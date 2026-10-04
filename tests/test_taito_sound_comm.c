#include "taito_sound_comm.h"

#include <assert.h>
#include <stdio.h>

typedef struct Trace {
    unsigned service_calls;
    unsigned nmi_calls;
    unsigned reset_calls;
    int nmi;
    int reset;
    int synchronization_pending;
} Trace;

static void service(void *opaque)
{
    ++((Trace *)opaque)->service_calls;
    ((Trace *)opaque)->synchronization_pending = 1; /* Request only; no inline slave run. */
}

static void nmi_line(void *opaque, int state)
{
    Trace *trace = (Trace *)opaque;
    ++trace->nmi_calls;
    trace->nmi = state;
}

static void reset_line(void *opaque, int state)
{
    Trace *trace = (Trace *)opaque;
    ++trace->reset_calls;
    trace->reset = state;
}

static uint8_t master_read(TaitoSoundComm *d, int effects)
{
    uint8_t v = 0xff;
    assert(taito_sound_comm_master_comm_read(d, effects, &v) == TAITO_SOUND_COMM_OK);
    return v;
}

static uint8_t slave_read(TaitoSoundComm *d, int effects)
{
    uint8_t v = 0xff;
    assert(taito_sound_comm_slave_comm_read(d, effects, &v) == TAITO_SOUND_COMM_OK);
    return v;
}

static void test_master_to_slave(void)
{
    Trace trace = {0};
    TaitoSoundCommConfig config = {nmi_line, reset_line, service, &trace};
    TaitoSoundComm *d = NULL;
    assert(taito_sound_comm_create(&config, &d) == TAITO_SOUND_COMM_OK);
    assert(d != NULL);

    /* Buffer a two-nibble command; writes keep only the low nibble. */
    taito_sound_comm_master_port_write(d, 0xf0);
    taito_sound_comm_master_comm_write(d, 0xfa);
    taito_sound_comm_master_comm_write(d, 0x1b);
    assert(trace.service_calls == 2);
    assert(trace.synchronization_pending);
    assert(taito_sound_comm_status(d) == 0x01);
    assert(trace.nmi == 0); /* NMI has not yet been enabled. */

    /* Slave mode 6 enables NMI, which reflects the pending incoming packet. */
    taito_sound_comm_slave_port_write(d, 6);
    taito_sound_comm_slave_comm_write(d, 0);
    assert(taito_sound_comm_nmi_enabled(d));
    assert(trace.nmi == 1);

    /* The host now services the requested barrier, after both master writes
     * returned. The slave sees the newly completed packet. */
    trace.synchronization_pending = 0;
    taito_sound_comm_slave_port_write(d, 0);
    assert(slave_read(d, 0) == 0x0a); /* debugger read leaves mode unchanged */
    assert(slave_read(d, 1) == 0x0a);
    assert(slave_read(d, 1) == 0x0b); /* mode 1 consumes and clears bit 0 */
    assert(taito_sound_comm_status(d) == 0);
    assert(trace.nmi == 0);

    /* Full status mode does not advance the mode selector. */
    taito_sound_comm_master_port_write(d, 4);
    assert(master_read(d, 1) == 0);
    assert(master_read(d, 1) == 0);
    assert(trace.service_calls == 4); /* one call per side-effectful master read */

    /* Master mode 4 drives the slave reset callback from the low nibble. */
    taito_sound_comm_master_comm_write(d, 0xf0);
    assert(trace.reset_calls == 1 && trace.reset == 0);
    taito_sound_comm_master_port_write(d, 4);
    taito_sound_comm_master_comm_write(d, 2);
    assert(trace.reset_calls == 2 && trace.reset == 1);
    assert(trace.service_calls == 6);

    /* Reset clears protocol state but deliberately does not synthesize line
     * transitions; the machine reset owner controls external CPU lines. */
    taito_sound_comm_reset(d);
    assert(taito_sound_comm_status(d) == 0);
    assert(!taito_sound_comm_nmi_enabled(d));
    assert(trace.reset_calls == 2 && trace.nmi_calls == 3);
    taito_sound_comm_master_port_write(d, 0xf);
    assert(master_read(d, 1) == 0);
    assert(trace.service_calls == 7);
    taito_sound_comm_destroy(d);
}

static void test_slave_to_master(void)
{
    Trace trace = {0};
    TaitoSoundCommConfig config = {nmi_line, reset_line, service, &trace};
    TaitoSoundComm *d = NULL;
    assert(taito_sound_comm_create(&config, &d) == TAITO_SOUND_COMM_OK);

    /* Slave sends a complete two-nibble reply. Master consumes it by reading
     * nibble 1, which clears the reply-full status bit. */
    taito_sound_comm_slave_port_write(d, 0);
    taito_sound_comm_slave_comm_write(d, 0x13);
    taito_sound_comm_slave_comm_write(d, 0x2e);
    assert(taito_sound_comm_status(d) == 0x04);
    taito_sound_comm_master_port_write(d, 0);
    assert(master_read(d, 0) == 0x03); /* side effects disabled: no service */
    assert(trace.service_calls == 0);
    assert(master_read(d, 1) == 0x03);
    assert(master_read(d, 1) == 0x0e);
    assert(taito_sound_comm_status(d) == 0);
    assert(trace.service_calls == 2);

    /* Nibble 2/3 uses status bit 3, and slave mode 4 is a no-op. */
    taito_sound_comm_slave_port_write(d, 2);
    taito_sound_comm_slave_comm_write(d, 0x0c);
    taito_sound_comm_slave_comm_write(d, 0x0d);
    assert(taito_sound_comm_status(d) == 0x08);
    taito_sound_comm_master_port_write(d, 2);
    assert(master_read(d, 1) == 0x0c);
    assert(master_read(d, 1) == 0x0d);
    assert(taito_sound_comm_status(d) == 0);
    taito_sound_comm_slave_port_write(d, 4);
    taito_sound_comm_slave_comm_write(d, 0xf);
    assert(taito_sound_comm_status(d) == 0);
    assert(trace.service_calls == 4);
    taito_sound_comm_destroy(d);
}

static void test_nmi_and_empty_config(void)
{
    Trace trace = {0};
    TaitoSoundCommConfig config = {nmi_line, reset_line, service, &trace};
    TaitoSoundComm *d = NULL;
    assert(taito_sound_comm_create(&config, &d) == TAITO_SOUND_COMM_OK);

    taito_sound_comm_master_port_write(d, 0);
    taito_sound_comm_master_comm_write(d, 0x11);
    taito_sound_comm_master_comm_write(d, 0x22);
    assert(taito_sound_comm_status(d) == 1);
    taito_sound_comm_slave_port_write(d, 5);
    taito_sound_comm_slave_comm_write(d, 0);
    assert(!taito_sound_comm_nmi_enabled(d) && trace.nmi == 0);
    taito_sound_comm_slave_port_write(d, 6);
    taito_sound_comm_slave_comm_write(d, 0);
    assert(taito_sound_comm_nmi_enabled(d) && trace.nmi == 1);
    taito_sound_comm_slave_port_write(d, 0);
    (void)slave_read(d, 1);
    (void)slave_read(d, 1);
    assert(trace.nmi == 0);
    taito_sound_comm_destroy(d);

    assert(taito_sound_comm_create(NULL, &d) == TAITO_SOUND_COMM_OK);
    taito_sound_comm_master_port_write(d, 0);
    taito_sound_comm_master_comm_write(d, 0x1f);
    taito_sound_comm_master_comm_write(d, 0x20);
    taito_sound_comm_slave_port_write(d, 0);
    assert(slave_read(d, 1) == 0x0f);
    assert(slave_read(d, 1) == 0);
    assert(taito_sound_comm_status(d) == 0);
    taito_sound_comm_destroy(d);
}

int main(void)
{
    TaitoSoundComm *d = NULL;
    uint8_t value;
    assert(taito_sound_comm_create(NULL, NULL) == TAITO_SOUND_COMM_ARGUMENT);
    assert(taito_sound_comm_create(NULL, &d) == TAITO_SOUND_COMM_OK);
    assert(taito_sound_comm_master_comm_read(d, 1, NULL) == TAITO_SOUND_COMM_ARGUMENT);
    assert(taito_sound_comm_slave_comm_read(NULL, 1, &value) == TAITO_SOUND_COMM_ARGUMENT);
    taito_sound_comm_destroy(d);

    test_master_to_slave();
    test_slave_to_master();
    test_nmi_and_empty_config();
    puts("Taito TC0140SYT/PC060HA command device tests passed");
    return 0;
}
