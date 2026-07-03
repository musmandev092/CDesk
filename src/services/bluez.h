/* bluez.h — Bluetooth adapter/device state via BlueZ on the system bus.
 *
 * Reads whether an adapter is powered and whether any device is connected,
 * cached for a few seconds so it can be polled from the render path cheaply.
 */
#ifndef DC_SERVICES_BLUEZ_H
#define DC_SERVICES_BLUEZ_H

#include <stdbool.h>
#include <stddef.h>

struct dc_dbus;

/* One paired/nearby device, from org.bluez.Device1's GetManagedObjects
 * properties (docs/13-POPOUTS-SPEC.md sec.1 bluetooth section). `mac` is the
 * colon-separated address recovered from the object path
 * (/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF), suitable for `bluetoothctl
 * connect/disconnect <mac>`. */
#define DC_BLUEZ_MAX_DEVICES 16

typedef struct dc_bluez_device {
    char mac[18];
    char name[64];
    bool paired;
    bool connected;
} dc_bluez_device;

typedef struct dc_bluez_info {
    bool available; /* BlueZ answered */
    bool powered;   /* an adapter is powered on */
    bool connected; /* at least one device is connected */

    dc_bluez_device devices[DC_BLUEZ_MAX_DEVICES];
    /* Paired-or-connected devices always; while a discovery is active
     * (dc_bluez_start_discovery(), W3.1), unpaired nearby devices are mixed
     * in too, `paired` distinguishing the two for the control-center's
     * "known" vs "nearby, tap to pair" row rendering. */
    int device_count;
} dc_bluez_info;

/* Bind the system bus (from dc_dbus). Call once at startup. Also exports and
 * registers dankc's org.bluez.Agent1 (W3.1 pairing flow, see the "pairing
 * agent" section below) if the system bus is available. */
void dc_bluez_init(struct dc_dbus *dbus);

/* Read cached BlueZ state (refreshed at most every few seconds). Returns true
 * if BlueZ is available. */
bool dc_bluez_read(dc_bluez_info *out);

/* Connect/disconnect a device by MAC, fire-and-forget (`bluetoothctl connect|
 * disconnect <mac>`, detached -- same run-detached shape as
 * services/audio.c's dc_audio_set_volume()). Only meaningful for already-
 * paired devices -- see dc_bluez_pair() below for first-time pairing. */
void dc_bluez_connect(const char *mac);
void dc_bluez_disconnect(const char *mac);

/* --- Discovery (control-center "Discover" affordance, W3.1) ---------------
 *
 * Adapter1.StartDiscovery/StopDiscovery on the system bus. While discovery is
 * active, dc_bluez_read()'s device list also includes unpaired nearby
 * devices BlueZ has advertised (see dc_bluez_info.device_count's doc above),
 * not just paired/connected ones. Idempotent (calling Start twice, or Stop
 * with none active, is a no-op). */
void dc_bluez_start_discovery(void);
void dc_bluez_stop_discovery(void);
bool dc_bluez_discovering(void);

/* --- Pairing a new (nearby, unpaired) device, W3.1 -------------------------
 *
 * Async: Device1.Pair() -> on success, Properties.Set("Trusted", true) ->
 * Device1.Connect(). One job at a time, same "replace the previous one"
 * convention as services/net.c's dc_net_wifi_connect_psk(). All three D-Bus
 * calls are fire-and-forget from the caller's perspective -- their replies
 * arrive asynchronously via the system bus's normal event-loop integration
 * (services/dbus.c), no extra polling/draining is needed to drive them
 * (unlike net.c's forked-process jobs). dc_bluez_pair_poll() just reports the
 * job's current state, which the async callbacks update as replies arrive.
 *
 * Pairing may need dankc's registered agent to answer BlueZ's
 * RequestConfirmation/RequestPasskey/etc -- see the "pairing agent" section
 * below; "Just Works" devices (most headphones/speakers) complete without
 * any agent callback at all.
 *
 * If DANKC_BT_DRYRUN is set (any value), no D-Bus calls are made: the
 * command sequence is logged via dc_info() instead, and the job resolves to
 * DC_BLUEZ_PAIR_SUCCESS on its own after a short simulated delay -- lets the
 * pairing UI be exercised without touching real hardware. Combined with
 * DANKC_BT_FAKE_DEVICE (below), pairing the fake device also synthesizes one
 * RequestConfirmation agent request so the confirm dialog itself can be
 * screenshotted end-to-end. */
typedef enum {
    DC_BLUEZ_PAIR_IDLE = 0,
    DC_BLUEZ_PAIR_IN_PROGRESS,
    DC_BLUEZ_PAIR_SUCCESS,
    DC_BLUEZ_PAIR_FAILED,
} dc_bluez_pair_state;

void dc_bluez_pair(const char *mac);

/* Non-blocking status read (safe to call every render frame). `mac_out`, if
 * non-NULL, is filled with the MAC of the device the current/last job
 * targeted (empty if none yet). `err_out`, if non-NULL, is filled with a
 * short reason on DC_BLUEZ_PAIR_FAILED only. */
dc_bluez_pair_state dc_bluez_pair_poll(char *mac_out, size_t mac_sz, char *err_out, size_t err_sz);

/* Abort the in-flight job (if any) and reset to DC_BLUEZ_PAIR_IDLE. Also
 * rejects (and clears) any pending agent request from the "pairing agent"
 * section below, so closing the bluetooth panel mid-pairing never leaves
 * BlueZ waiting on a reply we'll never send. */
void dc_bluez_pair_reset(void);

/* --- Pairing agent (org.bluez.Agent1, W3.1) --------------------------------
 *
 * Registered once at dc_bluez_init() time with capability "KeyboardDisplay"
 * (RegisterAgent + RequestDefaultAgent on org.bluez.AgentManager1). If
 * another agent already owns the registration (a desktop environment's own
 * bluetooth applet, or a second dankc instance), registration fails and is
 * logged -- dankc keeps running with pairing confined to devices that don't
 * need any agent interaction ("Just Works"), same graceful-degradation
 * contract as services/polkit.c's authentication agent.
 *
 * At most one agent request is ever pending at a time (BlueZ serializes
 * pairing attempts the same way dc_bluez_pair() does). The control center
 * polls dc_bluez_agent_poll() every render frame while the bluetooth section
 * is open and, on a pending request, shows an inline confirm/passkey-entry
 * panel (reusing the exact inline-field pattern from the Wi-Fi password
 * panel, W1.1) that calls one of the respond functions below once the user
 * answers. */
typedef enum {
    DC_BLUEZ_AGENT_NONE = 0,
    DC_BLUEZ_AGENT_CONFIRM,    /* RequestConfirmation: "does NNNNNN match?" */
    DC_BLUEZ_AGENT_AUTHORIZE,  /* RequestAuthorization: plain "pair with X?" */
    DC_BLUEZ_AGENT_PASSKEY,    /* RequestPasskey: type the code shown on the device */
} dc_bluez_agent_kind;

typedef struct dc_bluez_agent_request {
    dc_bluez_agent_kind kind;
    char device_name[64];
    char passkey_str[8]; /* "NNNNNN", only meaningful for DC_BLUEZ_AGENT_CONFIRM */
} dc_bluez_agent_request;

/* True (and `out` filled) while a request is awaiting a UI answer. */
bool dc_bluez_agent_poll(dc_bluez_agent_request *out);

/* Answer a pending DC_BLUEZ_AGENT_CONFIRM/DC_BLUEZ_AGENT_AUTHORIZE request.
 * No-op if none is pending. */
void dc_bluez_agent_respond_yesno(bool accept);

/* Answer a pending DC_BLUEZ_AGENT_PASSKEY request with the digits the user
 * typed, or NULL/"" to cancel. No-op if none is pending. */
void dc_bluez_agent_respond_passkey(const char *digits);

#endif /* DC_SERVICES_BLUEZ_H */
