# virtio-sound: specification extract (research notes, no driver exists)

Extracted from OASIS virtio v1.2 section 5.14 and the QEMU docs/source by a read-only research pass. Every fact carries its source URL below; items marked UNVERIFIED are not confirmed and must be checked before any driver relies on them.

# virtio-snd (Sound Device, ID 25) spec notes

Fetched 2026-11-23 with curl, converted HTML to text, quoted verbatim below (whitespace normalised).

Sources (short tags used beside each fact):
- [S12] https://docs.oasis-open.org/virtio/virtio/v1.2/csd01/virtio-v1.2-csd01.html (section 5.14 "Sound Device", 2.4, 3.1.1, 4.2.2, 7.3.17)
- [S13] https://docs.oasis-open.org/virtio/virtio/v1.3/csd01/virtio-v1.3-csd01.html (section 5.14, for diffs vs 1.2)
- [Q-RST] https://gitlab.com/qemu-project/qemu/-/raw/master/docs/system/devices/virtio/virtio-snd.rst
- [Q-C] https://gitlab.com/qemu-project/qemu/-/raw/master/hw/audio/virtio-snd.c
- [Q-H] https://gitlab.com/qemu-project/qemu/-/raw/master/include/hw/audio/virtio-snd.h
- [Q-PCI] https://gitlab.com/qemu-project/qemu/-/raw/master/hw/audio/virtio-snd-pci.c
- [Q-AUD] https://gitlab.com/qemu-project/qemu/-/raw/master/include/audio/audio.h
- [Q-QAPI] https://gitlab.com/qemu-project/qemu/-/raw/master/qapi/audio.json

All multi-byte fields are little-endian (le32/le64) per the struct definitions. Struct sizes marked "(computed)" are my arithmetic from the field lists, not stated in the spec.

---

## 1. Device ID  [S12 5.14.1]
> 5.14.1 Device ID
> 25

On virtio-mmio this is the value read from the DeviceID register (0x008); a value of 0 means no device (general MMIO rule, not quoted here; UNVERIFIED wording).

## 2. Virtqueues  [S12 5.14.2]
> 0 controlq
> 1 eventq
> 2 txq
> 3 rxq
> The control queue is used for sending control messages from the driver to the device.
> The event queue is used for sending notifications from the device to the driver.
> The tx queue is used to send PCM frames for output streams.
> The rx queue is used to receive PCM frames from input streams.

Queue sizes: the spec does NOT define sizes; the driver reads QueueNumMax per queue. [S12 4.2.2]:
> QueueNumMax 0x034 R ... Reading from the register returns the maximum size (number of elements) of the queue the device is ready to process or zero (0x0) if the queue is not available.
> The driver MUST write a value to QueueNum which is less than or equal to the value presented by the device in QueueNumMax.

QEMU implementation: all four queues are created with size 64 [Q-C, virtio_snd_realize]:
`virtio_add_queue(vdev, 64, virtio_snd_handle_ctrl)` (same 64 for event, tx, rx).

## 3. Feature bits
- v1.2 [S12 5.14.3]: "None currently defined."
- v1.3 [S13 5.14.3]: "VIRTIO_SND_F_CTLS (0) Device supports control elements." (adds the `controls` config field and VIRTIO_SND_R_CTL_* = 0x0300.. requests: CTL_INFO, CTL_ENUM_ITEMS, CTL_READ, CTL_WRITE, CTL_TLV_READ, CTL_TLV_WRITE, CTL_TLV_COMMAND).
- QEMU offers no device-specific feature bits; get_features comment quotes "virtio-v1.2-csd01, 5.14.3, Feature Bits None currently defined." [Q-C]. Driver should still negotiate VIRTIO_F_VERSION_1 (generic; not re-quoted here).

Per-stream "features" (VIRTIO_SND_PCM_F_*) and per-jack features are NOT device feature bits; they live in the info structs (see 6, 8).

## 4. Device configuration layout  [S12 5.14.4]
```c
struct virtio_snd_config {
    le32 jacks;
    le32 streams;
    le32 chmaps;
};
```
> jacks (driver-read-only) indicates a total number of all available jacks.
> streams (driver-read-only) indicates a total number of all available PCM streams.
> chmaps (driver-read-only) indicates a total number of all available channel maps.

Offsets: jacks 0x0, streams 0x4, chmaps 0x8 (computed). On MMIO, device config begins at 0x100.
v1.3 adds `le32 controls;` at 0xC, and "The driver MUST NOT read the controls field if VIRTIO_SND_F_CTLS has not been negotiated." [S13 5.14.4, 5.14.5.1]

## 5. Initialization and reset

### Device-specific init  [S12 5.14.5]
> Configure the control, event, tx and rx queues.
> Read the jacks field and send a control request to query information about the available jacks.
> Read the streams field and send a control request to query information about the available PCM streams.
> Read the chmaps field and send a control request to query information about the available channel maps.
> Populate the event queue with empty buffers.

### Driver requirements  [S12 5.14.5.1]
> The driver MUST populate the event queue with empty buffers of at least the struct virtio_snd_event size.
> The driver MUST NOT put a device-readable buffers in the event queue.

### Generic driver init sequence  [S12 3.1.1]
> The driver MUST follow this sequence to initialize a device:
> Reset the device.
> Set the ACKNOWLEDGE status bit: the guest OS has noticed the device.
> Set the DRIVER status bit: the guest OS knows how to drive the device.
> Read device feature bits, and write the subset of feature bits understood by the OS and driver to the device. During this step the driver MAY read (but MUST NOT write) the device-specific configuration fields to check that it can support the device before accepting it.
> Set the FEATURES_OK status bit. The driver MUST NOT accept new feature bits after this step.
> Re-read device status to ensure the FEATURES_OK bit is still set: otherwise, the device does not support our subset of features and the device is unusable.
> Perform device-specific setup, including discovery of virtqueues for the device, optional per-bus setup, reading and possibly writing the device's virtio configuration space, and population of virtqueues.
> Set the DRIVER_OK status bit. At this point the device is "live".
> If any of these steps go irrecoverably wrong, the driver SHOULD set the FAILED status bit ...

### Reset  [S12 2.4, 4.2.2]
> The driver may want to initiate a device reset at various times; notably, it is required to do so during device initialization and device cleanup. The mechanism used by the driver to initiate the reset is transport specific.
> A device MUST reinitialize device status to 0 after receiving a reset.
> A device MUST NOT send notifications or interact with the queues after indicating completion of the reset by reinitializing device status to 0, until the driver re-initializes the device.
> The driver SHOULD consider a driver-initiated reset complete when it reads device status as 0.
MMIO: "Status 0x070 RW ... Writing zero (0x0) to this register triggers a device reset." [S12 4.2.2]

The sound section defines no sound-specific reset rules. QEMU's virtio_snd_reset drops queued/in-flight state [Q-C, virtio_snd_reset]; exact behaviour not reviewed line by line (UNVERIFIED detail).

## 6. Common header, request codes, status codes, events  [S12 5.14.6]
```c
enum {
    /* jack control request types */
    VIRTIO_SND_R_JACK_INFO = 1,
    VIRTIO_SND_R_JACK_REMAP,                 /* 2 */
    /* PCM control request types */
    VIRTIO_SND_R_PCM_INFO = 0x0100,
    VIRTIO_SND_R_PCM_SET_PARAMS,             /* 0x0101 */
    VIRTIO_SND_R_PCM_PREPARE,                /* 0x0102 */
    VIRTIO_SND_R_PCM_RELEASE,                /* 0x0103 */
    VIRTIO_SND_R_PCM_START,                  /* 0x0104 */
    VIRTIO_SND_R_PCM_STOP,                   /* 0x0105 */
    /* channel map control request types */
    VIRTIO_SND_R_CHMAP_INFO = 0x0200,
    /* jack event types */
    VIRTIO_SND_EVT_JACK_CONNECTED = 0x1000,
    VIRTIO_SND_EVT_JACK_DISCONNECTED,        /* 0x1001 */
    /* PCM event types */
    VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED = 0x1100,
    VIRTIO_SND_EVT_PCM_XRUN,                 /* 0x1101 */
    /* common status codes */
    VIRTIO_SND_S_OK = 0x8000,
    VIRTIO_SND_S_BAD_MSG,                    /* 0x8001 */
    VIRTIO_SND_S_NOT_SUPP,                   /* 0x8002 */
    VIRTIO_SND_S_IO_ERR                      /* 0x8003 */
};
/* a common header */
struct virtio_snd_hdr { le32 code; };                         /* 4 bytes (computed) */
/* an event notification */
struct virtio_snd_event { struct virtio_snd_hdr hdr; le32 data; };  /* 8 bytes (computed) */
```
(Numeric comments are implicit C enum increments, computed.)

> A generic control message consists of a request part and a response part.
> A request part has, or consists of, a common header containing the following device-readable field: code specifies a device request type (VIRTIO_SND_R_*).
> A response part has, or consists of, a common header containing the following device-writable field: code indicates a device request status (VIRTIO_SND_S_*).
> VIRTIO_SND_S_OK - success.
> VIRTIO_SND_S_BAD_MSG - a control message is malformed or contains invalid parameters.
> VIRTIO_SND_S_NOT_SUPP - requested operation or parameters are not supported.
> VIRTIO_SND_S_IO_ERR - an I/O error occurred.
> The request part may be followed by an additional device-readable payload, and the response part may be followed by an additional device-writable payload.

Descriptor chain for a control message therefore = device-readable request (+payload) then device-writable response (virtio_snd_hdr + optional payload).

Direction enum:
```c
enum { VIRTIO_SND_D_OUTPUT = 0, VIRTIO_SND_D_INPUT };
```

## 7. Item information request (JACK_INFO / PCM_INFO / CHMAP_INFO)  [S12 5.14.6.1-5.14.6.2]
```c
struct virtio_snd_query_info {       /* 16 bytes (computed) */
    struct virtio_snd_hdr hdr;       /* VIRTIO_SND_R_*_INFO */
    le32 start_id;
    le32 count;
    le32 size;
};
struct virtio_snd_info { le32 hda_fn_nid; };   /* common header of every info struct */
```
> size specifies the size of the structure containing information for one item (used for backward compatibility).
> The response consists of the virtio_snd_hdr structure (contains the request status code), followed by the device-writable information structures of the item.
> The driver MUST NOT set start_id and count such that start_id + count is greater than the total number of particular items that is indicated in the device configuration space.
> The driver MUST provide a buffer of sizeof(struct virtio_snd_hdr) + count * size bytes for the response.

## 8. Jacks  [S12 5.14.6.4-5.14.6.5]
```c
struct virtio_snd_jack_hdr { struct virtio_snd_hdr hdr; le32 jack_id; };   /* 8 (computed) */
enum { VIRTIO_SND_JACK_F_REMAP = 0 };
struct virtio_snd_jack_info {        /* 24 bytes (computed) */
    struct virtio_snd_info hdr;
    le32 features; /* 1 << VIRTIO_SND_JACK_F_XXX */
    le32 hda_reg_defconf;
    le32 hda_reg_caps;
    u8 connected;
    u8 padding[7];
};
struct virtio_snd_jack_remap {       /* 16 bytes (computed) */
    struct virtio_snd_jack_hdr hdr; /* .code = VIRTIO_SND_R_JACK_REMAP */
    le32 association;
    le32 sequence;
};
```
> connected indicates the current jack connection status (1 - connected, 0 - disconnected).
> The device MUST initialize the padding bytes to 0.
Jack events: hdr = VIRTIO_SND_EVT_JACK_CONNECTED / _DISCONNECTED; "data indicates a jack identifier from 0 to jacks - 1."

## 9. PCM control messages  [S12 5.14.6.6]
```c
struct virtio_snd_pcm_hdr { struct virtio_snd_hdr hdr; le32 stream_id; };  /* 8 (computed) */
```
> stream_id specifies a PCM stream identifier from 0 to streams - 1.

PREPARE, RELEASE, START, STOP requests are just `virtio_snd_pcm_hdr` (no extra payload; the spec gives no other struct for them):
> 5.14.6.6.4 VIRTIO_SND_R_PCM_PREPARE Prepare a stream with specified stream ID.
> 5.14.6.6.5 VIRTIO_SND_R_PCM_RELEASE Release a stream with specified stream ID.
> 5.14.6.6.6 VIRTIO_SND_R_PCM_START Start a stream with specified stream ID.
> 5.14.6.6.7 VIRTIO_SND_R_PCM_STOP Stop a stream with specified stream ID.
Response for all: `virtio_snd_hdr` (4 bytes).

Release rules [S12 5.14.6.6.5.1]:
> The device MUST complete all pending I/O messages for the specified stream ID.
> The device MUST NOT complete the control request while there are pending I/O messages for the specified stream ID.

### Lifecycle  [S12 5.14.6.6.1]
> SET PARAMETERS ... Possible valid transitions: set parameters, prepare.
> PREPARE ... Possible valid transitions: set parameters, prepare, start, release. Output only: the driver transfers data for pre-buffing.
> START ... Possible valid transitions: stop. The driver transfers data to/from the stream.
> STOP ... Possible valid transitions: start, release.
> RELEASE ... Possible valid transitions: set parameters, prepare.
QEMU returns VIRTIO_SND_S_BAD_MSG on unexpected-state SET_PARAMS [Q-C].

### PCM_INFO  [S12 5.14.6.6.2]
```c
enum { VIRTIO_SND_PCM_F_SHMEM_HOST = 0, VIRTIO_SND_PCM_F_SHMEM_GUEST, VIRTIO_SND_PCM_F_MSG_POLLING,
       VIRTIO_SND_PCM_F_EVT_SHMEM_PERIODS, VIRTIO_SND_PCM_F_EVT_XRUNS };
enum { /* width / physical width */
    VIRTIO_SND_PCM_FMT_IMA_ADPCM = 0, /* 4/4 */  VIRTIO_SND_PCM_FMT_MU_LAW,  /*1*/ VIRTIO_SND_PCM_FMT_A_LAW, /*2*/
    VIRTIO_SND_PCM_FMT_S8, /*3*/  VIRTIO_SND_PCM_FMT_U8, /*4*/  VIRTIO_SND_PCM_FMT_S16, /*5: 16/16*/
    VIRTIO_SND_PCM_FMT_U16, /*6*/ VIRTIO_SND_PCM_FMT_S18_3, /*7*/ VIRTIO_SND_PCM_FMT_U18_3, /*8*/
    VIRTIO_SND_PCM_FMT_S20_3, /*9*/ VIRTIO_SND_PCM_FMT_U20_3, /*10*/ VIRTIO_SND_PCM_FMT_S24_3, /*11*/
    VIRTIO_SND_PCM_FMT_U24_3, /*12*/ VIRTIO_SND_PCM_FMT_S20, /*13*/ VIRTIO_SND_PCM_FMT_U20, /*14*/
    VIRTIO_SND_PCM_FMT_S24, /*15*/ VIRTIO_SND_PCM_FMT_U24, /*16*/ VIRTIO_SND_PCM_FMT_S32, /*17*/
    VIRTIO_SND_PCM_FMT_U32, /*18*/ VIRTIO_SND_PCM_FMT_FLOAT, /*19*/ VIRTIO_SND_PCM_FMT_FLOAT64, /*20*/
    VIRTIO_SND_PCM_FMT_DSD_U8, /*21*/ VIRTIO_SND_PCM_FMT_DSD_U16, /*22*/ VIRTIO_SND_PCM_FMT_DSD_U32, /*23*/
    VIRTIO_SND_PCM_FMT_IEC958_SUBFRAME /*24*/ };
enum { VIRTIO_SND_PCM_RATE_5512 = 0, _8000 /*1*/, _11025 /*2*/, _16000 /*3*/, _22050 /*4*/, _32000 /*5*/,
       _44100 /*6*/, _48000 /*7*/, _64000 /*8*/, _88200 /*9*/, _96000 /*10*/, _176400 /*11*/,
       _192000 /*12*/, _384000 /*13*/ };
struct virtio_snd_pcm_info {        /* 32 bytes (computed) */
    struct virtio_snd_info hdr;     /* off 0 */
    le32 features; /* 1 << VIRTIO_SND_PCM_F_XXX */   /* off 4 */
    le64 formats;  /* 1 << VIRTIO_SND_PCM_FMT_XXX */ /* off 8 */
    le64 rates;    /* 1 << VIRTIO_SND_PCM_RATE_XXX */ /* off 16 */
    u8 direction;  /* off 24 */
    u8 channels_min;
    u8 channels_max;
    u8 padding[5];
};
```
(Numeric index comments are computed from enum order; the spec text shows only the "= 0" anchors. RATE names abbreviated here; full name is VIRTIO_SND_PCM_RATE_<n>.)
> Only interleaved samples are supported.
> The device MUST NOT set undefined feature, format, rate and direction values. The device MUST initialize the padding bytes to 0.

### PCM_SET_PARAMS  [S12 5.14.6.6.3]
```c
struct virtio_snd_pcm_set_params {  /* 24 bytes (computed) */
    struct virtio_snd_pcm_hdr hdr; /* .code = VIRTIO_SND_R_PCM_SET_PARAMS */  /* off 0 */
    le32 buffer_bytes;   /* off 8 */
    le32 period_bytes;   /* off 12 */
    le32 features; /* 1 << VIRTIO_SND_PCM_F_XXX */  /* off 16 */
    u8 channels;         /* off 20 */
    u8 format;           /* VIRTIO_SND_PCM_FMT_* */
    u8 rate;             /* VIRTIO_SND_PCM_RATE_* */
    u8 padding;
};
```
Note: `format` and `rate` are enum INDEXES (u8), not bitmasks.
Feature semantics in set_params:
> VIRTIO_SND_PCM_F_SHMEM_HOST - use shared memory allocated by the host (is a placeholder and MUST NOT be selected at the moment),
> VIRTIO_SND_PCM_F_SHMEM_GUEST - ... (is a placeholder and MUST NOT be selected at the moment),
> VIRTIO_SND_PCM_F_MSG_POLLING - suppress available buffer notifications for tx and rx queues (device should poll virtqueue),
> VIRTIO_SND_PCM_F_EVT_SHMEM_PERIODS - enable elapsed period notifications for shared memory transport,
> VIRTIO_SND_PCM_F_EVT_XRUNS - enable underrun/overrun notifications.
Device: "If the device has an intermediate buffer, its size MUST be no less than the specified buffer_bytes value."
Driver [5.14.6.6.3.2]:
> period_bytes MUST be a divider buffer_bytes, i.e. buffer_bytes % period_bytes == 0.
> The driver MUST NOT set undefined feature, format and rate values.
> The driver MUST NOT set the feature, format, and rate that are not specified in the stream configuration.
> The driver MUST NOT set the channels value as less than the channels_min or greater than the channels_max values specified in the stream configuration.
> The driver MUST NOT set the VIRTIO_SND_PCM_F_SHMEM_HOST and VIRTIO_SND_PCM_F_SHMEM_GUEST bits at the same time.
> The driver MUST initialize the padding byte to 0.
> If the bits associated with the shared memory are not selected, the driver MUST use the tx and rx queues for data transfer (see PCM I/O Messages).

## 10. PCM notifications (eventq)  [S12 5.14.6.7]
> To enable notifications, the driver must negotiate these features using the set stream parameters request
> VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED - a hardware buffer period has elapsed, the period size is controlled using the period_bytes field.
> VIRTIO_SND_EVT_PCM_XRUN - an underflow for the output stream or an overflow for the input stream has occurred.
> data indicates a PCM stream identifier from 0 to streams - 1.
Event queue buffer = device-writable `struct virtio_snd_event` (8 bytes); driver posts empty ones (see 5).

## 11. PCM I/O messages (txq / rxq)  [S12 5.14.6.8]
> An I/O message consists of the header part, followed by the buffer, and then the status part.
```c
struct virtio_snd_pcm_xfer   { le32 stream_id; };                     /* 4 bytes, device-readable */
struct virtio_snd_pcm_status { le32 status; le32 latency_bytes; };    /* 8 bytes, device-writable */
```
> status contains VIRTIO_SND_S_OK if an operation is successful, and VIRTIO_SND_S_IO_ERR otherwise.
> latency_bytes indicates the current device latency.
> Since all buffers in the queue (with one exception) should be of the size period_bytes, the completion of such an I/O request can be considered an elapsed period notification.

Output (txq) chain: [xfer (R)] [PCM frames (R)] [status (W)].
> In case of an output stream, the header is followed by a device-readable buffer containing PCM frames for writing to the device. All messages are placed into the tx queue.
> Device: The device MUST NOT complete the I/O request until the buffer is totally consumed.
> Driver: The driver SHOULD populate the tx queue with period_bytes sized buffers. The only exception is the end of the stream. The driver MUST NOT place device-writable buffers into the tx queue.
(The last statement conflicts literally with the device-writable status trailer; the intended meaning is that the PCM data buffer must not be device-writable. Interpretation is mine, UNVERIFIED.)

Input (rxq) chain: [xfer (R)] [PCM frames (W)] [status (W)].
> A used descriptor specifies the length of the buffer that was written by the device. It should be noted that the length value contains the size of the virtio_snd_pcm_status structure plus the size of the recorded frames.
> Device: The device MUST NOT complete the I/O request until the buffer is full. The only exception is the end of the stream.
> Driver: The driver SHOULD populate the rx queue with period_bytes sized empty buffers before starting the stream. The driver MUST NOT place device-readable buffers into the rx queue.
(Same caveat: the xfer header is device-readable; interpretation UNVERIFIED.)

## 12. Channel maps  [S12 5.14.6.9]
```c
#define VIRTIO_SND_CHMAP_MAX_SIZE 18
struct virtio_snd_chmap_info {        /* 24 bytes (computed) */
    struct virtio_snd_info hdr;
    u8 direction;
    u8 channels;
    u8 positions[VIRTIO_SND_CHMAP_MAX_SIZE];
};
```
Positions enum: NONE=0, NA=1, MONO=2, FL=3, FR=4, RL=5, RR=6, FC=7, LFE=8, SL=9, SR=10, RC=11, FLC, FRC, RLC, RRC, FLW, FRW, FLH, FCH, FRH, TC, TFL, TFR, TFC, TRL, TRR, TRC, TFLC, TFRC, TSL, TSR, LLFE, RLFE, BC, BLC, BRC (values computed by enum order).
> The device MUST NOT set the channels value to more than VIRTIO_SND_CHMAP_MAX_SIZE.

## 13. Conformance list  [S12 7.3.17]
Device MUST conform to 5.14.6.4.1.1, 5.14.6.6.2.1, 5.14.6.6.3.1, 5.14.6.6.5.1, 5.14.6.8.1.1, 5.14.6.8.2.1, 5.14.6.9.1.1. (The 1.2 conformance clause lists no driver requirements for sound.)

---

## 14. QEMU device  [Q-RST, Q-C, Q-H, Q-PCI, Q-AUD, Q-QAPI]
- MMIO type name: `#define TYPE_VIRTIO_SND "virtio-sound-device"` [Q-H]. PCI wrapper: `"virtio-sound-pci"` [Q-PCI]. Built when CONFIG_VIRTIO_SND (+ CONFIG_VIRTIO_PCI for pci) [hw/audio/meson.build].
- Properties [Q-C virtio_snd_properties]: `audiodev` (via `DEFINE_AUDIO_PROPERTIES` -> `DEFINE_PROP_AUDIODEV("audiodev", ...)` [Q-AUD]), `jacks` (default 0), `streams` (default 2), `chmaps` (default 0).
- Realize limits [Q-C]: jacks > 8 rejected; streams < 1 or > 10 rejected; chmaps > VIRTIO_SND_CHMAP_MAX_SIZE rejected.
- Doc [Q-RST]: "jacks number of physical jacks (Unimplemented)", "chmaps number of channel maps (Unimplemented)", "the first one will always be a playback stream, an optional second will always be a capture stream", "All streams are stereo and have the default channel positions Front left, right."
  Code actually assigns direction as `i < streams/2 + (streams & 1) ? OUTPUT : INPUT` (first half output), which differs from the doc's "cycle" wording for >2 streams [Q-C]. With default streams=2: stream 0 = output, stream 1 = input.
- Stream info reported [Q-C]: features = 0, hda_fn_nid = 0, channels_min = 1, channels_max = 2.
  formats = S8|U8|S16|U16|S32|U32|FLOAT; rates = all 14 (5512..384000).
- SET_PARAMS checks [Q-C]: channels 1..AUDIO_MAX_CHANNELS else NOT_SUPP; unsupported format/rate -> NOT_SUPP; features not offered -> BAD_MSG; buffer_bytes > 500 MiB -> BAD_MSG.
- No device feature bits offered (comment cites v1.2 "None currently defined") [Q-C].
- The QEMU .rst doc only shows PCI and `-audio ...,model=virtio` examples; it does NOT document virtio-mmio or the wav backend.

### wav audiodev  [Q-QAPI]
`AudiodevWavOptions`: `in`, `out`, `path` -- "@path: name of the wav file to record (default 'qemu.wav')".

Derived command line (NOT in QEMU docs, not executed -> UNVERIFIED):
```
-audiodev wav,id=snd0,path=/tmp/aurora.wav \
-device virtio-sound-device,audiodev=snd0,streams=1
```
Requires a machine with a virtio-mmio bus (e.g. aarch64/riscv `-M virt`, or x86 `-M microvm`); whether AuroraOS's machine type exposes virtio-mmio slots is UNVERIFIED. With `-M virt` the mmio transport is legacy unless `-global virtio-mmio.force-legacy=false` is given (UNVERIFIED for this setup; QEMU default behaviour from memory).

## UNVERIFIED summary
- Spec defines no virtqueue sizes; 64 is QEMU-specific.
- Exact semantics of "MUST NOT place device-writable buffers into the tx queue" vs the writable status trailer (interpretation only).
- Sound-specific reset behaviour (spec has none; QEMU reset internals not audited).
- Any QEMU mmio command line, wav file output, force-legacy behaviour, and machine-type mmio availability (not run, per instructions).
- Enum numeric values beyond the explicit "=" anchors are computed from C enum ordering.
