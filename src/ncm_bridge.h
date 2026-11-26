#ifndef NCM_BRIDGE_H
#define NCM_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

struct carplay_vehicle_state;
struct ncm_airplay_status {
    int active;
    int info_complete;
    int display_width;
    int display_height;
    int requested_width;
    int requested_height;
    int physical_width;
    int physical_height;
    int display_density;
    int requested_fps;
    int actual_fps;
    int native_size;
    char display_error[192];
    int touch;
    int keyboard;
    int media_audio;
    int microphone;
    int audio_streams;
    int media_available;
    int microphone_available;
    int microphone_ready;
    int microphone_demand;
    uint64_t microphone_packets_received;
    int streaming;
    int oem_icon_present;
    int request_car_ui_supported;
    int car_ui_ready;
};


/* Opens a dual-stack TUN and configures fe80::1 and 169.254.0.1. */
int ncm_bridge_open(const char *ifname);
/*
 * Host role: configure the netdev that belongs to the head unit.
 *
 * Once the 0x51 hand-off has put us on the bus as host, the car's own CDC-NCM
 * function is the one that matters, and the kernel's cdc_ncm driver is what
 * binds it.  That netdev is found through sysfs -- device under `usb_dev_name`
 * (a sysfs name such as "2-1") plus driver cdc_ncm -- never by name, and never
 * through our own gadget layout: functions/ncm.usb0 describes what we offer
 * while we are the *device* and says nothing about this side.
 *
 * Starts interface-scoped mDNS discovery when ready. Discovered endpoints use
 * the legacy profile; pending/active iAP2 endpoints share the same connector.
 * Returns 0 when a netdev was found and addressed, -1 otherwise (logged).
 */
int ncm_bridge_car_netdev_up(const char *usb_dev_name, unsigned wait_ms);
/* Call with 0 before starting each Host iAP2 round, then with the current
 * iap2_session_authentication_ready result. Early discovery is retained until ready.
 * Returns 1 on authorization loss: caller must retire/join the Host round.
 * stop/close clears the gate; gadget and bench paths keep their behavior. */
int ncm_bridge_host_authentication(int authenticated);
/*
 * Detects the kernel-side NCM function (configfs "ncm.usb0") and switches the
 * bridge into netdev mode for this process.
 *
 * FunctionFS on this 4.4 kernel rejects CS_INTERFACE descriptors, so interfaces
 * 2/3 could never carry the four functional descriptors a real iPhone
 * advertises.  When the kernel function is available the supervisor links it
 * into the same configuration: the kernel emits those descriptors and hands us
 * an ordinary Ethernet netdev, and this module stops owning the NCM endpoints.
 *
 * Must be called before the descriptors are written, because the answer decides
 * whether they describe interfaces 2/3 at all.  Returns 1 in netdev mode.
 */
int ncm_bridge_kernel_available(void);
/*
 * Reads the name the kernel gave the NCM netdev. configfs
 * functions/ncm.usb0/ifname is the authority -- the attribute is read-only, so
 * whatever it reports is what register_netdev() used. Falls back to scanning
 * /sys/class/net for "usb*", then to the compiled-in guess. Returns 0 when the
 * value came from a real source, -1 when it is only the guess.
 */
int ncm_bridge_kernel_ifname(char *out, size_t len);
/*
 * Brings the kernel netdev up and gives it the same addresses the TUN had, so
 * the AirPlay connect path and the policy routes keep working unchanged.
 * Idempotent: safe to call on every bind and every re-enable.
 */
int ncm_bridge_netdev_up(void);
/*
 * Same as ncm_bridge_netdev_up() but never waits. Used at startup, because the
 * netdev is registered when the UDC is bound -- before any host shows up -- and
 * stalling startup for a netdev that is not there yet helps nobody.
 */
int ncm_bridge_netdev_prepare(void);
/* Starts USB-OUT -> TUN and TUN -> USB-IN workers for one USB bind. */
int ncm_bridge_start(int usb_out_fd, int usb_in_fd);
/* Restarts only the bulk endpoint workers on a repeated FunctionFS ENABLE. */
int ncm_bridge_restart_usb(int usb_out_fd, int usb_in_fd);
/*
 * Starts one asynchronous AirPlay TCP connect attempt. Valid IPv4 addresses are
 * tried before IPv6 addresses, while IPv6 remains available as a fallback.
 * iAP2 0x4301 callers retain the HAP profile. Scoped IPv6 must identify the
 * current NCM interface; other-interface zones are rejected.
 */
int ncm_bridge_connect_endpoint(const char *const addresses[], size_t count,
				uint16_t port);
/*
 * Bench: dial a fixed AirPlay endpoint with no USB transport.
 *
 * Used with a simulated head unit (the CarPlay receiver simulator, or any
 * AirPlay receiver that speaks the legacy sender path) so the whole RTSP /
 * plist / screen / audio stack can be exercised without a car.  Nothing above
 * the socket is stubbed.  The caller is responsible for keeping the process
 * alive; the daemon's --airplay-endpoint flag does exactly that.
 *
 * CARPLAY_AIRPLAY_PROFILE=legacy selects the legacy profile for this bench
 * entry point only. Default remains HAP. Returns 0 when the connect was queued,
 * -1 when it was refused (logged).
 */
int ncm_bridge_bench_start(const char *address, uint16_t port);
/* Disables discovery callbacks, stops/joins discovery and connection/stream
 * workers, leaving the TUN available for the next USB bind. No discovery join
 * holds the bridge mutex. Configured media/video options are retained. */
void ncm_bridge_stop(void);
/* Stops workers and closes the TUN. */
void ncm_bridge_close(void);
int ncm_bridge_is_running(void);
/* Configures the next AirPlay session; values are copied under the bridge lock. */
void ncm_bridge_set_airplay_options(int enable_media, int enable_microphone,
                                    int display_width, int display_height,
                                    int physical_width, int physical_height,
                                    int display_density, int video_fps,
                                    int video_bitrate);
void ncm_bridge_set_vehicle_state(struct carplay_vehicle_state *state,
                                  uint64_t generation);
void ncm_bridge_get_airplay_status(struct ncm_airplay_status *status);
/*
 * Sends requestUI through the active encrypted AirPlay event channel. The
 * existing USB, screen, microphone and audio session is left untouched.
 * Returns 0 when sent, 1 when the event channel is not ready, 2 when the
 * receiver did not advertise enhancedRequestCarUI, and -1 for I/O failure.
 */
int ncm_bridge_request_car_ui(void);
/*
 * Takes the peer's hardware address from iAP2 0x4301 deviceIdentifier.
 *
 * Learning the peer MAC from received frames is not reliable: a head unit
 * that has not configured its NCM interface yet sends frames sourced from the
 * NCM default host address 02:00:00:00:00:02, and if we latch onto that every
 * unicast frame we send is filtered by the real NIC. deviceIdentifier carries
 * the actual address, so it wins over anything learned from a frame.
 *
 * Returns 0 on success, -1 when the string is not a usable unicast MAC.
 */
int ncm_bridge_set_peer_mac(const char *identifier);

#endif
