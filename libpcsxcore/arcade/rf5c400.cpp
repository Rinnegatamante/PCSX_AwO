/* SPDX-License-Identifier: BSD-3-Clause
 * copyright-holder: Ville Linde; improvements by the hoot development team.
 * Standalone adaptation of MAME src/devices/sound/rf5c400.cpp/.h at
 * deb2f54fe7300e7a192690318beb860159bfd52a. See rf5c400_LICENSE.txt.
 */
#include "rf5c400.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>

static int16_t signed16(uint16_t v)
{ return int16_t(v <= 32767 ? int32_t(v) : int32_t(v)-65536); }
static int32_t arithmetic_shift9(int32_t v)
{ return v >= 0 ? v / 512 : -int32_t((uint32_t(-v) + 511) / 512); }
using std::pow;
using std::sqrt;
enum { TYPE_MASK=0xc0, TYPE_16=0, TYPE_8LOW=0x40, TYPE_8HIGH=0x80 };
enum { PHASE_NONE=0, PHASE_ATTACK, PHASE_DECAY, PHASE_RELEASE };
struct sound_stream {
    int32_t output[4] = {};
    int samples() const { return 1; }
    void add_int(int channel, int, int32_t sample, int) { output[channel] += sample; }
};
struct RF5C400 {
    using rf5c400_channel = RF5C400Channel;
	class envelope_tables
	{
	public:
		envelope_tables();
		void init(uint32_t clock);
		double ar(rf5c400_channel const &chan) const { return m_ar[decode80(chan.attack >> 8)]; }
		double dr(rf5c400_channel const &chan) const { return m_dr[decode80(chan.decay >> 8)]; }
		double rr(rf5c400_channel const &chan) const { return m_rr[decode80(chan.release >> 8)]; }
	private:
		static constexpr uint8_t decode80(uint8_t val) { return (val & 0x80) ? ((val & 0x7f) + 0x1f) : val; }
		double m_ar[0x9f];
		double m_dr[0x9f];
		double m_rr[0x9f];
	};

    RF5C400Config config{};
    uint32_t clock_hz=0, sample_rate=0;
    uint64_t cycles=0, clock_frames=0, rendered_frames=0, sample_phase=0;
    bool busy=false;
    int volume_table[256]{};
    double pan_table[256]{};
    envelope_tables m_env_tables;
    rf5c400_channel m_channels[32]{};
    uint16_t m_rf5c400_status=0;
    uint32_t m_ext_mem_address=0;
    uint16_t m_ext_mem_data=0, m_req_channel=0;
    uint16_t read_word(uint32_t address) {
        address &= RF5C400_ADDRESS_MASK & ~1u;
        if (config.read_word) return config.read_word(config.memory_opaque, address);
        if (size_t(address)+1 >= config.sample_rom_size) return 0xffff;
        return uint16_t(config.sample_rom[address] | uint16_t(config.sample_rom[address+1]) << 8);
    }
    void write_word(uint32_t address, uint16_t value) {
        if (config.write_word)
            config.write_word(config.memory_opaque, address & (RF5C400_ADDRESS_MASK & ~1u), value);
    }
    void sound_stream_update(sound_stream &stream);
    uint16_t rf5c400_r(uint32_t offset, uint16_t mask);
    void rf5c400_w(uint32_t offset, uint16_t data, uint16_t mask);
    void frame(int32_t *out) {
        sound_stream stream;
        sound_stream_update(stream);
        rendered_frames++;
        if (out) std::copy(std::begin(stream.output),std::end(stream.output),out);
    }
};
RF5C400::envelope_tables::envelope_tables()
{
	std::fill(std::begin(m_ar), std::end(m_ar), 0.0);
	std::fill(std::begin(m_dr), std::end(m_dr), 0.0);
	std::fill(std::begin(m_rr), std::end(m_rr), 0.0);
}

void RF5C400::envelope_tables::init(uint32_t clock)
{
	/* envelope parameter (experimental) */
	static constexpr double ENV_AR_SPEED    = 0.1;
	static constexpr int    ENV_MIN_AR      = 0x02;
	static constexpr int    ENV_MAX_AR      = 0x80;
	static constexpr double ENV_DR_SPEED    = 2.0;
	static constexpr int    ENV_MIN_DR      = 0x20;
	static constexpr int    ENV_MAX_DR      = 0x73;
	static constexpr double ENV_RR_SPEED    = 0.7;
	static constexpr int    ENV_MIN_RR      = 0x20;
	static constexpr int    ENV_MAX_RR      = 0x54;

	double r;

	// attack
	r = 1.0 / (ENV_AR_SPEED * (clock / 384));
	for (int i = 0; i < ENV_MIN_AR; i++)
		m_ar[i] = 1.0;
	for (int i = ENV_MIN_AR; i < ENV_MAX_AR; i++)
		m_ar[i] = r * (ENV_MAX_AR - i) / (ENV_MAX_AR - ENV_MIN_AR);
	for (int i = ENV_MAX_AR; i < 0x9f; i++)
		m_ar[i] = 0.0;

	// decay
	r = -5.0 / (ENV_DR_SPEED * (clock / 384));
	for (int i = 0; i < ENV_MIN_DR; i++)
		m_dr[i] = r;
	for (int i = ENV_MIN_DR; i < ENV_MAX_DR; i++)
		m_dr[i] = r * (ENV_MAX_DR - i) / (ENV_MAX_DR - ENV_MIN_DR);
	for (int i = ENV_MAX_DR; i < 0x9f; i++)
		m_dr[i] = 0.0;

	// release
	r = -5.0 / (ENV_RR_SPEED * (clock / 384));
	for (int i = 0; i < ENV_MIN_RR; i++)
		m_rr[i] = r;
	for (int i = ENV_MIN_RR; i < ENV_MAX_RR; i++)
		m_rr[i] = r * (ENV_MAX_RR - i) / (ENV_MAX_RR - ENV_MIN_RR);
	for (int i = ENV_MAX_RR; i < 0x9f; i++)
		m_rr[i] = 0.0;
}


void RF5C400::sound_stream_update(sound_stream &stream)
{
	int i, ch;
	uint64_t start, end, loop;
	uint64_t pos;
	uint8_t vol, lvol, rvol, effect_lvol, effect_rvol, type;
	uint8_t env_phase;
	double env_level, env_step, env_rstep;

	for (ch=0; ch < 32; ch++)
	{
		rf5c400_channel *channel = &m_channels[ch];

		start = ((uint32_t)(channel->startH & 0xFF00) << 8) | channel->startL;
		end = ((uint32_t)(channel->endHloopH & 0xFF) << 16) | channel->endL;
		loop = ((uint32_t)(channel->endHloopH & 0xFF00) << 8) | channel->loopL;
		pos = channel->pos;
		vol = channel->volume & 0xFF;
		lvol = channel->pan & 0xFF;
		rvol = channel->pan >> 8;
		effect_lvol = channel->effect & 0xFF;
		effect_rvol = channel->effect >> 8;
		type = (channel->volume >> 8) & TYPE_MASK;

		env_phase = channel->env_phase;
		env_level = channel->env_level;
		env_step = channel->env_step;
		env_rstep = env_step * channel->env_scale;

		if (start == end)
		{
			// This occurs in pop'n music when trying to play a non-existent sample on the sound test menu
			continue;
		}

		for (i=0; i < stream.samples(); i++)
		{
			int16_t tmp;
			int32_t sample;

			if (env_phase == PHASE_NONE) break;

			tmp = signed16(read_word(uint32_t((pos>>16)<<1)));
			switch ( type )
			{
				case TYPE_16:
					sample = tmp;
					break;
				case TYPE_8LOW:
					sample = signed16(uint16_t(uint16_t(tmp) << 8));
					break;
				case TYPE_8HIGH:
					sample = (int16_t)(tmp & 0xFF00);
					break;
				default:
					sample = 0;
					break;
			}

			if ( sample & 0x8000 )
			{
				sample ^= 0x7FFF;
			}

			env_level += env_rstep;
			switch (env_phase)
			{
			case PHASE_ATTACK:
				if (env_level >= 1.0)
				{
					env_phase = PHASE_DECAY;
					env_level = 1.0;
					if ((channel->decay & 0x0080) || (channel->decay == 0x100))
					{
						env_step = 0.0;
					}
					else
					{
						env_step = m_env_tables.dr(*channel);
					}
					env_rstep = env_step * channel->env_scale;
				}
				break;
			case PHASE_DECAY:
				if (env_level <= 0.0)
				{
					env_phase = PHASE_NONE;
					env_level = 0.0;
					env_step = 0.0;
					env_rstep = 0.0;
				}
				break;
			case PHASE_RELEASE:
				if (env_level <= 0.0)
				{
					env_phase = PHASE_NONE;
					env_level = 0.0;
					env_step = 0.0;
					env_rstep = 0.0;
				}
				break;
			}

			sample *= volume_table[vol];
			sample = int32_t(arithmetic_shift9(sample) * env_level);
			stream.add_int(0, i, sample * pan_table[lvol], 32768);
			stream.add_int(1, i, sample * pan_table[rvol], 32768);
			stream.add_int(2, i, sample * pan_table[effect_lvol], 32768);
			stream.add_int(3, i, sample * pan_table[effect_rvol], 32768);

			pos += channel->step;
			if ((pos>>16) > end)
			{
				pos -= loop<<16;
				pos &= 0xFFFFFF0000ULL;

				if (pos < (start<<16))
				{
					// This case only shows up in Firebeat games from what I could tell.
					// The loop value will be higher than the actual buffer size.
					// This is used when DMAs will be overwriting the current buffer.
					// It expects the buffer to be looped without any additional commands.
					pos = start<<16;
				}
			}

		}
		channel->pos = pos;

		channel->env_phase = env_phase;
		channel->env_level = env_level;
		channel->env_step = env_step;
	}
}

uint16_t RF5C400::rf5c400_r(uint32_t offset, uint16_t /* mem_mask */)
{
	if (offset < 0x400)
	{
		//osd_printf_debug("%s:rf5c400_r: %08X, %08X\n", machine().describe_context(), offset, mem_mask);

		switch(offset)
		{
			case 0x00:
			{
				return m_rf5c400_status;
			}

			case 0x04:      // unknown read
			{
				return 0;
			}

			case 0x09:      // position read?
			{
				// The game will always call rf5c400_w 0x08 with a channel number and some other value before reading this register.
				// The call to rf5c400_w 0x08 contains additional information, potentially what information it's expecting to be returned here.
				// This implementation assumes all commands want the same information as command 6.
				/* Clock synchronization is performed by the standalone caller. */

				rf5c400_channel* channel = &m_channels[m_req_channel];
				if (channel->env_phase == PHASE_NONE)
				{
					return 0;
				}

				// pop'n music's SPU program expects to read this register 6 times with the same value between every read before it will send the next DMA request.
				//
				// This register is polled while a streaming BGM is being played.
				// For pop'n music specifically, the game starts off by reading 0x200000 into 0x00780000 - 0x00880000.
				// When 2xxx is found (pos - start = 0x00080000), it will trigger the next DMA of 0x100000 overwriting 0x00780000 - 0x00800000, and continues polling the register until it reads 1xxx next.
				// When 1xxx is found (pos - start = 0x00040000), it will trigger the next DMA of 0x100000 overwriting 0x00800000 - 0x00880000, and continues polling the register until it reads 2xxx next.
				// ... repeat until song is finished, alternating between 2xxx and 1xxx ...
				// This ends up so that it'll always be buffering new sample data into the sections of memory that aren't being used.
				auto start = ((uint32_t)(channel->startH & 0xFF00) << 8) | channel->startL;
				auto ch_offset = (channel->pos >> 16) - start;
				return ch_offset >> 6;
			}

			case 0x13:      // memory read
			{
				return read_word(m_ext_mem_address<<1);
			}

			default:
			{
				//osd_printf_debug("%s:rf5c400_r: %08X, %08X\n", machine().describe_context(), offset, mem_mask);
				return 0;
			}
		}
	}
	else
	{
		//int ch = (offset >> 5) & 0x1f;
		int reg = (offset & 0x1f);

		switch (reg)
		{
		case 0x0F:      // unknown read
			return 0;

		default:
			return 0;
		}
	}
}

void RF5C400::rf5c400_w(uint32_t offset, uint16_t data, uint16_t /* mem_mask */)
{
	if (offset < 0x400)
	{
		switch(offset)
		{
			case 0x00:
			{
				m_rf5c400_status = data;
				break;
			}

			case 0x01:      // channel control
			{
				int ch = data & 0x1f;
				switch ( data & 0x60 )
				{
					case 0x60:
						m_channels[ch].pos =
							((uint32_t)(m_channels[ch].startH & 0xFF00) << 8) | m_channels[ch].startL;
						m_channels[ch].pos <<= 16;

						m_channels[ch].env_phase = PHASE_ATTACK;
						m_channels[ch].env_level = 0.0;
						m_channels[ch].env_step  = m_env_tables.ar(m_channels[ch]);
						break;
					case 0x40:
						if (m_channels[ch].env_phase != PHASE_NONE)
						{
							m_channels[ch].env_phase = PHASE_RELEASE;
							if (m_channels[ch].release & 0x0080)
							{
								m_channels[ch].env_step = 0.0;
							}
							else
							{
								m_channels[ch].env_step = m_env_tables.rr(m_channels[ch]);
							}
						}
						break;
					default:
						m_channels[ch].env_phase = PHASE_NONE;
						m_channels[ch].env_level = 0.0;
						m_channels[ch].env_step  = 0.0;
						break;
				}
				break;
			}

			case 0x08:
			{
				// There's some other data stuffed in the upper bits beyond the channel: data >> 5
				// The other data might be some kind of register or command. I've seen 0, 4, 5, and 6.
				// Firebeat uses 6 when polling rf5c400_r 0x09.
				m_req_channel = data & 0x1f;
				break;
			}

			case 0x09:      // relative to env attack (0x0c00/ 0x1c00/ 0x1e00)

			case 0x11:      // memory r/w address, bits 15 - 0
			{
				m_ext_mem_address &= ~0xffff;
				m_ext_mem_address |= data;
				break;
			}
			case 0x12:      // memory r/w address, bits 23 - 16
			{
				m_ext_mem_address &= 0xffff;
				m_ext_mem_address |= (uint32_t)(data) << 16;
				break;
			}
			case 0x13:      // memory write data
			{
				m_ext_mem_data = data;
				break;
			}

			case 0x14:      // memory write
			{
				if ((data & 0x3) == 3)
				{
					write_word(m_ext_mem_address << 1, m_ext_mem_data);
				}
				break;
			}

			case 0x21:      // reverb(character).w
			case 0x32:      // reverb(pre-lpf).w
			case 0x2B:      // reverb(level).w
			case 0x20:      // ???.b : reverb(time).b

			case 0x2C:      // chorus(level).w
			case 0x30:      // chorus(rate).w
			case 0x22:      // chorus(macro).w
			case 0x23:      // chorus(depth).w
			case 0x24:      // chorus(macro).w
			case 0x2F:      // chorus(depth).w
			case 0x27:      // chorus(send level to reverb).w

			default:
			{
				//osd_printf_debug("%s:rf5c400_w: %08X, %08X, %08X\n", machine().describe_context(), data, offset, mem_mask);
				break;
			}
		}
		//osd_printf_debug("%s:rf5c400_w: %08X, %08X, %08X\n", machine().describe_context(), data, offset, mem_mask);
	}
	else
	{
		// channel registers
		int ch = (offset >> 5) & 0x1f;
		int reg = (offset & 0x1f);

		rf5c400_channel *channel = &m_channels[ch];

		switch (reg)
		{
			case 0x00:      // sample start address, bits 23 - 16
			{
				channel->startH = data;
				break;
			}
			case 0x01:      // sample start address, bits 15 - 0
			{
				channel->startL = data;
				break;
			}
			case 0x02:      // sample playing frequency
			{
				channel->step = ((data & 0x1fff) << (data >> 13)) * 4;
				channel->freq = data;
				break;
			}
			case 0x03:      // sample end address, bits 15 - 0
			{
				channel->endL = data;
				break;
			}
			case 0x04:      // sample end address, bits 23 - 16 , sample loop 23 - 16
			{
				channel->endHloopH = data;
				break;
			}
			case 0x05:      // sample loop offset, bits 15 - 0
			{
				channel->loopL = data;
				break;
			}
			case 0x06:      // channel volume
			{
				channel->pan = data;
				break;
			}
			case 0x07:      // effect depth
			{
				// 0xCCRR: CC = chorus send depth, RR = reverb send depth

				// Note: beatmania III uses this register differently?
				//  When effects are off, it writes 0xe0e0 to this register:
				//    Changed ch 30 reg 6 to volume 18176 -> l: 71, r: 0
				//    Changed ch 30 reg 7 to volume 57568 -> 0xe0e0
				//    Changed ch 31 reg 6 to volume 71    -> l: 0, r: 71
				//    Changed ch 31 reg 7 to volume 57568 -> 0xe0e0
				//  When effects are on, it writes reg 6 information here:
				//    Changed ch 30 reg 6 to volume 57568 -> 0xe0e0
				//    Changed ch 30 reg 7 to volume 18176 -> l: 71, r: 0
				//    Changed ch 31 reg 6 to volume 57568 -> 0xe0e0
				//    Changed ch 31 reg 7 to volume 71    -> l: 0, r: 71
				//
				// I've observed values of 0xff instead of 0xe0 in some games like Gradius 4,
				// so 0xe0 is not some kind of max value.
				//
				// When effects are enabled the audio is redirected to an external
				// PCB board for effect processing before returning to the sound PCB.
				//
				// That makes me think that the effects are not internal and that
				// this register is just used to adjust the volume of the channels
				// being routed externally.
				//
				// Whether the effect is a chorus or reverb or something else would
				// depend on how the PCB is using these external channels.
				channel->effect = data;
				break;
			}
			case 0x08:      // volume, flag
			{
				channel->volume = data;
				break;
			}
			case 0x09:      // env attack
			{
				// 0x0100: max speed                  (in case of attack <= 0x40)
				// 0xXX40: XX = attack-0x3f (encoded) (in case of attack > 0x40)
				//
				channel->attack = data;
				break;
			}
			case 0x0A:      // relative to env attack ?
			{
				// always 0x0100/0x140
				break;
			}
			case 0x0B:      // relative to env decay ?
			{
				// always 0x0100/0x140/0x180
				break;
			}
			case 0x0C:      // env decay
			{
				// 0xXX70: XX = decay (encoded) (in case of decay > 0x71)
				// 0xXX80: XX = decay (encoded) (in case of decay <= 0x71)
				channel->decay = data;
				break;
			}
			case 0x0D:      // relative to env release ?
			{
				// always 0x0100/0x140
				break;
			}
			case 0x0E:      // env release
			{
				// 0xXX70: XX = release-0x1f (encoded) (0x01 if release <= 0x20)
				channel->release = data;
				break;
			}
			case 0x0F:      // unknown write
			{
				// always 0x0000
				break;
			}
			case 0x10:      // resonance, cutoff freq.
			{
				// bit 15-12: resonance
				// bit 11-0 : cutoff frequency
				channel->cutoff = data;
				break;
			}
		}
	}
}

extern "C" {
int rf5c400_create(const RF5C400Config *cfg, RF5C400 **out)
{
    if (!cfg || !out) return RF5C400_ARGUMENT;
    *out=nullptr;
    const uint32_t clock=cfg->clock_hz ? cfg->clock_hz : RF5C400_DEFAULT_CLOCK;
    if (clock < 384 || (!cfg->read_word && (!cfg->sample_rom || !cfg->sample_rom_size)) ||
        (cfg->sample_rom_size & 1) || cfg->sample_rom_size > 0x2000000u)
        return RF5C400_ARGUMENT;
    RF5C400 *d=new (std::nothrow) RF5C400;
    if (!d) return RF5C400_NOMEM;
    d->config=*cfg; d->clock_hz=clock; d->sample_rate=clock/384;
    d->cycles=cfg->cycle_origin;
    d->m_env_tables.init(clock);
    double maximum=255.0;
    for (unsigned i=0;i<256;i++) {
        d->volume_table[i]=uint16_t(maximum);
        maximum/=pow(10.0,(4.5/(256.0/16.0))/20.0);
    }
    for (unsigned i=0;i<0x48;i++) d->pan_table[i]=sqrt(double(0x47-i))/sqrt(0x47);
    for (auto &ch:d->m_channels) ch.env_scale=1.0;
    *out=d;
    return RF5C400_OK;
}
void rf5c400_destroy(RF5C400 *d) { if (d && !d->busy) delete d; }
int rf5c400_reset(RF5C400 *d)
{ return !d ? RF5C400_ARGUMENT : d->busy ? RF5C400_REENTRANT : RF5C400_OK; }
int rf5c400_read(RF5C400 *d, uint32_t off, uint16_t mask, uint16_t *out)
{
    if (!d || !out || off>=0x800) return RF5C400_ARGUMENT;
    if (d->busy) return RF5C400_REENTRANT;
    d->busy=true; *out=d->rf5c400_r(off,mask); d->busy=false;
    return RF5C400_OK;
}
int rf5c400_write(RF5C400 *d, uint32_t off, uint16_t value, uint16_t mask)
{
    if (!d || off>=0x800) return RF5C400_ARGUMENT;
    if (d->busy) return RF5C400_REENTRANT;
    d->busy=true; d->rf5c400_w(off,value,mask); d->busy=false;
    return RF5C400_OK;
}
int rf5c400_advance_to_cycle(RF5C400 *d, uint64_t target)
{
    if (!d) return RF5C400_ARGUMENT;
    if (d->busy) return RF5C400_REENTRANT;
    if (target<d->cycles) return RF5C400_TIME_REWIND;
    d->busy=true;
    while (d->cycles<target) {
        const uint64_t until_frame=(d->clock_hz-d->sample_phase+d->sample_rate-1)/d->sample_rate;
        const uint64_t elapsed=std::min(until_frame,target-d->cycles);
        d->cycles+=elapsed; d->sample_phase+=elapsed*d->sample_rate;
        if (d->sample_phase>=d->clock_hz) {
            int32_t output[4]; d->sample_phase-=d->clock_hz; d->clock_frames++;
            d->frame(output);
            if (d->config.sample) d->config.sample(d->config.sample_opaque,output,d->cycles);
        }
    }
    d->busy=false;
    return RF5C400_OK;
}
int rf5c400_run_cycles(RF5C400 *d, uint64_t count)
{
    if (!d) return RF5C400_ARGUMENT;
    if (d->busy) return RF5C400_REENTRANT;
    if (count>UINT64_MAX-d->cycles) return RF5C400_OVERFLOW;
    return rf5c400_advance_to_cycle(d,d->cycles+count);
}
int rf5c400_render(RF5C400 *d, size_t count, int32_t *out, size_t capacity)
{
    if (!d || (out && (capacity<count || count>SIZE_MAX/(4*sizeof(*out))))) return RF5C400_ARGUMENT;
    if (d->busy) return RF5C400_REENTRANT;
    if (count>UINT64_MAX-d->rendered_frames) return RF5C400_OVERFLOW;
    d->busy=true;
    for (size_t i=0;i<count;i++) d->frame(out ? out+i*4 : nullptr);
    d->busy=false;
    return RF5C400_OK;
}
int rf5c400_get_channel(const RF5C400 *d, unsigned ch, RF5C400Channel *out)
{
    if (!d || !out || ch>=32) return RF5C400_ARGUMENT;
    *out=d->m_channels[ch]; return RF5C400_OK;
}
void rf5c400_get_status(const RF5C400 *d, RF5C400Status *s)
{
    if (!s) return;
    *s={}; if (!d) return;
    s->clock_hz=d->clock_hz;s->sample_rate=d->sample_rate;
    s->cycles=d->cycles;s->clock_frames=d->clock_frames;s->rendered_frames=d->rendered_frames;
    s->sample_phase=d->sample_phase;s->status=d->m_rf5c400_status;
    s->external_address=d->m_ext_mem_address;s->external_data=d->m_ext_mem_data;
    s->requested_channel=d->m_req_channel;
}
}
