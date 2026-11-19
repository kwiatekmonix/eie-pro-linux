// SPDX-License-Identifier: GPL-2.0-only

#include <linux/init.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/version.h>

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/rawmidi.h>

MODULE_DESCRIPTION("Akai EIE pro driver");
MODULE_AUTHOR("Michal Rydlo <michal.rydlo@gmail.com>");
MODULE_LICENSE("GPL v2");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

static DEFINE_MUTEX(devices_mutex);
static unsigned int devices_used;
static struct usb_driver eie_driver;

#define SYNC_URB_CNT 2

#define PLAY_URB_CNT 2
#define PLAY_PKT_CNT 40

#define CAP_URB_CNT 4
#define CAP_BUF_SIZE 4096

#define MIN_URB_CNT 2
#define MOUT_URB_CNT 2

#define BYTES_PER_FRAME 12

/*
 * TODO: redefine states & respect the close command again
 * TODO: limit the rate at open() to the rate of the running stream; prepare
 *       refuses a different rate (-EBUSY) while the other stream is RUNNING
 *       and eie->rate != 0 (with eie->rate == 0 the guard does not refuse),
 *       but a PREPARED (not yet started) stream and a trigger START racing
 *       with reset_eie() are not protected
 * TODO: correctly handle xruns
 */

enum {
	PLAYBACK_RUNNING,
	CAPTURE_RUNNING,
	URBS_FLOWING,
	DISCONNECTED,
	MIN_OPEN,
	MIN_UP
};

struct eie_playback_urb {
	struct eie *eie;
	struct urb *urb;
	bool silent;
	unsigned int len; /* in frames */
};

struct eie {
	struct usb_device *udev;
	struct usb_interface *ifa;
	struct usb_interface *ifb;

	struct snd_card *card;
	unsigned int card_index;

	struct snd_pcm *pcm;

	unsigned int rate;

	/* process context only: URB start/stop (reset_eie), disconnect */
	struct mutex mutex;

	__u8 sync_endpoint_addr;
	size_t sync_packet_size;
	struct urb *sync_urbs[SYNC_URB_CNT];

	__u8 play_endpoint_addr;
	size_t play_packet_size;

	struct eie_playback_urb play_urbs[PLAY_URB_CNT];
	struct snd_pcm_substream *play_substream;
	wait_queue_head_t urbs_flow_wait;
	/* callbacks between the pointer copy and the end of its use */
	atomic_t play_in_cb;
	atomic_t cap_in_cb;
	wait_queue_head_t cb_wait;

	unsigned int play_buf_pos;
	unsigned int played_frames;
	unsigned char wanted_idx;

	__u8 cap_endpoint_addr;
	struct urb *cap_urbs[CAP_URB_CNT];
	size_t cap_buf_size;
	struct snd_pcm_substream *cap_substream;
	unsigned int cap_buf_pos;
	unsigned int cap_frames;

	atomic_t frames_elapsed; /**< frames elapsed as reported by EIE */

	spinlock_t lock; /**< used for TODO */

	unsigned long states;

	struct snd_rawmidi *rmidi;
	__u8 min_endpoint_addr;
	__u8 mout_endpoint_addr;
	struct urb *min_urbs[MIN_URB_CNT];
	size_t min_buf_size;
	struct urb *mout_urbs[MOUT_URB_CNT];
	size_t mout_buf_size;
	unsigned long submitted_mout_urbs;
	struct snd_rawmidi_substream *min_substream;
	struct snd_rawmidi_substream *mout_substream;
};

static struct snd_pcm_hardware eie_playback_hw = {
	.info = (SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BATCH |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_FIFO_IN_FRAMES),
	.formats = SNDRV_PCM_FMTBIT_S24_3LE,
	.rates = (SNDRV_PCM_RATE_44100 |
		SNDRV_PCM_RATE_48000 |
		SNDRV_PCM_RATE_88200 |
		SNDRV_PCM_RATE_96000),
	.rate_min = 44100,
	.rate_max = 96000,
	.channels_min = 4,
	.channels_max = 4,
	.buffer_bytes_max = 45000 * 1024, /* TODO: clarify, copied from ua101 */
	.period_bytes_min = 64*BYTES_PER_FRAME,
	.period_bytes_max = UINT_MAX,
	.periods_min = 2,
	.periods_max = UINT_MAX,
};

static void kill_all_urbs(struct eie *eie);
static int submit_init_play_urbs(struct eie *eie);

static int eie_set_alt_setting(struct eie *eie)
{
	int err;

	err = usb_set_interface(eie->udev, 0, 1);
	if (err == 0)
		err = usb_set_interface(eie->udev, 1, 1);
	return err;
}

static int eie_prepare_hw(struct snd_pcm_substream *substream)
{
	int err;

	/* TODO: determine possible HW params from runnnig streams. */
	substream->runtime->hw = eie_playback_hw;
	err = snd_pcm_hw_constraint_minmax(substream->runtime,
		SNDRV_PCM_HW_PARAM_BUFFER_TIME, 10*1000, UINT_MAX);
	return err;
}

static int eie_ppcm_open(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	unsigned long flags;
	int err;

	err = eie_prepare_hw(substream);
	if (err < 0)
		return err;
	spin_lock_irqsave(&eie->lock, flags);
	eie->play_substream = substream;
	spin_unlock_irqrestore(&eie->lock, flags);
	return 0;
}

static int eie_cpcm_open(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	unsigned long flags;
	int err;

	err = eie_prepare_hw(substream);
	if (err < 0)
		return err;
	spin_lock_irqsave(&eie->lock, flags);
	eie->cap_substream = substream;
	spin_unlock_irqrestore(&eie->lock, flags);
	return 0;
}

static int eie_ppcm_close(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	unsigned long flags;

	spin_lock_irqsave(&eie->lock, flags);
	clear_bit(PLAYBACK_RUNNING, &eie->states);
	eie->play_substream = NULL;
	spin_unlock_irqrestore(&eie->lock, flags);

	return 0;
}

static int eie_cpcm_close(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	unsigned long flags;

	spin_lock_irqsave(&eie->lock, flags);
	clear_bit(CAPTURE_RUNNING, &eie->states);
	eie->cap_substream = NULL;
	spin_unlock_irqrestore(&eie->lock, flags);

	return 0;
}

static int eie_pcm_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *hw_params)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	return snd_pcm_lib_alloc_vmalloc_buffer(substream,
		params_buffer_bytes(hw_params));
#else
	struct snd_pcm_runtime *runtime = substream->runtime;
	size_t size = params_buffer_bytes(hw_params);

	/* Allocate the PCM runtime buffer, which is exposed to userspace. */
	if (runtime->dma_area) {
		if (runtime->dma_bytes >= size)
			return 0; /* already large enough */
		vfree(runtime->dma_area);
	}
	runtime->dma_area = vzalloc(size);
	if (!runtime->dma_area)
		return -ENOMEM;
	runtime->dma_bytes = size;
	return 1;
#endif
}

static int eie_pcm_hw_free(struct snd_pcm_substream *substream)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	return snd_pcm_lib_free_vmalloc_buffer(substream);
#else
	struct snd_pcm_runtime *runtime = substream->runtime;

	vfree(runtime->dma_area);
	runtime->dma_area = NULL;
	return 0;
}

static struct page *eie_get_page(struct snd_pcm_substream *substream,
	unsigned long offset)
{
	return vmalloc_to_page(substream->runtime->dma_area + offset);
}
#endif

static const char *usb_error_string(int err)
{
	switch (err) {
	case -ENODEV:
		return "no device";
	case -ENOENT:
		return "endpoint not enabled";
	case -EPIPE:
		return "endpoint stalled";
	case -ENOSPC:
		return "not enough bandwidth";
	case -ESHUTDOWN:
		return "device disabled";
	case -EHOSTUNREACH:
		return "device suspended";
	case -ETIMEDOUT:
		return "operation timed out";
	case -EINVAL:
	case -EAGAIN:
	case -EFBIG:
	case -EMSGSIZE:
		return "internal error";
	default:
		return "unknown error";
	}
}

static int submit_init_sync_urbs(struct eie *eie)
{
	int err;
	int i;

	for (i = 0; i < SYNC_URB_CNT; i++) {
		err = usb_submit_urb(eie->sync_urbs[i], GFP_KERNEL);
		if (err < 0) {
			dev_err(&eie->udev->dev, "USB request error %d %s",
				err, usb_error_string(err));
			return err;
		}
	}

	dev_dbg(&eie->udev->dev, "Submitted sync urbs.");

	return 0;
}

static int submit_init_cap_urbs(struct eie *eie)
{
	int err;
	int i;

	for (i = 0; i < CAP_URB_CNT; i++) {
		err = usb_submit_urb(eie->cap_urbs[i], GFP_KERNEL);
		if (err < 0) {
			dev_err(&eie->udev->dev, "USB request error %d %s",
				err, usb_error_string(err));
			return err;
		}
	}

	dev_dbg(&eie->udev->dev, "Submitted capture urbs.");

	return 0;
}

struct magic_seq {
	__u8 type;
	__u8 request;
	__u16 value;
	__u16 index;
	__u16 size;
};

static struct magic_seq magic_seq1[] = {
	{0xc0, 86, 0, 0, 3},
	{0xc0, 86, 0, 0, 5},
	{0xc0, 73, 0, 0, 1},
	{0xa2, 129, 0x0100, 0, 3},
	{0, 0, 0, 0, 0}
};

static struct magic_seq magic_seq2[] = {
	{0x22, 1, 0x0100, 134, 3},
	{0x22, 1, 0x0100, 2, 3},
	{0x22, 1, 0x0100, 134, 3},

	{0xa2, 129, 0x0100, 134, 3},
	{0xc0, 73, 0, 0, 1},
	{0x40, 73, 0x0032, 0, 0},
	{0, 0, 0, 0, 0}
};

#define MAX_MAGIC_SEQ_LENGTH 5 /* enough to fit any magic sequence buffer */

static int send_magic_sequence(struct eie *eie, struct magic_seq *m, char *data)
{
	int err;

	WARN_ON(m->size > MAX_MAGIC_SEQ_LENGTH);

	while (m->type != 0) {
		unsigned int p = m->type & 0x80 ? usb_rcvctrlpipe(eie->udev, 0)
			: usb_sndctrlpipe(eie->udev, 0);

		dev_dbg(&eie->udev->dev, "Sending control transfer. %x %u %x %u",
			m->type, m->request, m->value, m->index);
		err = usb_control_msg(eie->udev, p, m->request, m->type,
			m->value, m->index, data, m->size, 1000);
		if (err < 0) {
			dev_dbg(&eie->udev->dev, "Result of control transfer. %d %s",
				err, usb_error_string(err));
			return err;
		}

		m++;
	}

	return 0;
}

static int reset_eie(struct eie *eie, unsigned int rate)
{
	int err = 0;
	unsigned long flags;
	unsigned char *data;

	if (test_bit(DISCONNECTED, &eie->states))
		return -ENODEV;

	data = kmalloc(MAX_MAGIC_SEQ_LENGTH, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	dev_dbg(&eie->udev->dev, "Resetting device");
	kill_all_urbs(eie);
	/* a failure below must make the next prepare redo the reset */
	spin_lock_irqsave(&eie->lock, flags);
	eie->rate = 0;
	spin_unlock_irqrestore(&eie->lock, flags);
	eie_set_alt_setting(eie);

	dev_dbg(&eie->udev->dev, "Starting magic initialization sequence.");

	err = send_magic_sequence(eie, &magic_seq1[0], data);
	if (err < 0)
		goto out;

	*((__le32 *) data) = __cpu_to_le32(rate);
	dev_dbg(&eie->udev->dev, "rate: %d data: %02x %02x %02x",
		rate, data[0], data[1], data[2]);

	err = send_magic_sequence(eie, &magic_seq2[0], data);
	if (err < 0)
		goto out;

	dev_dbg(&eie->udev->dev, "Completed magic initialization sequence.");

	spin_lock_irqsave(&eie->lock, flags);
	eie->rate = rate;
	spin_unlock_irqrestore(&eie->lock, flags);

	/* all URBs are dead here; wait for a fresh first play URB below */
	clear_bit(URBS_FLOWING, &eie->states);

	err = submit_init_sync_urbs(eie);
	if (err < 0)
		goto out;

	err = submit_init_play_urbs(eie);
	if (err < 0)
		goto out;

	err = submit_init_cap_urbs(eie);
	if (err < 0)
		goto out;

	if (!wait_event_timeout(eie->urbs_flow_wait,
				test_bit(URBS_FLOWING, &eie->states) ||
				test_bit(DISCONNECTED, &eie->states), HZ)) {
		dev_err(&eie->udev->dev, "Timeout waiting for URBs to start\n");
		err = -ETIMEDOUT;
	} else if (test_bit(DISCONNECTED, &eie->states)) {
		err = -ENODEV;
	}

out:
	/* make the next prepare retry the reset */
	if (err) {
		spin_lock_irqsave(&eie->lock, flags);
		eie->rate = 0;
		spin_unlock_irqrestore(&eie->lock, flags);
	}

	kfree(data);
	return err;
}

/* called with eie->mutex held */
static int eie_prepare_rate(struct eie *eie, unsigned int rate,
			    int other_running_bit)
{
	unsigned long flags;
	bool busy;

	spin_lock_irqsave(&eie->lock, flags);
	busy = test_bit(other_running_bit, &eie->states) &&
	       eie->rate != 0 && eie->rate != rate;
	spin_unlock_irqrestore(&eie->lock, flags);
	if (busy) {
		dev_info_ratelimited(&eie->udev->dev,
				     "Rate %u refused: other stream runs at %u\n",
				     rate, READ_ONCE(eie->rate));
		return -EBUSY;
	}

	if (rate == READ_ONCE(eie->rate))
		return 0;

	return reset_eie(eie, rate);
}

static int eie_ppcm_prepare(struct snd_pcm_substream *substream)
{
	int err = 0;
	unsigned long flags;
	struct eie *eie = substream->private_data;

	dev_dbg(&eie->udev->dev, "play: prepare rate=%u eie->rate=%u\n",
		substream->runtime->rate, READ_ONCE(eie->rate));
	mutex_lock(&eie->mutex);
	err = eie_prepare_rate(eie, substream->runtime->rate, CAPTURE_RUNNING);
	mutex_unlock(&eie->mutex);

	spin_lock_irqsave(&eie->lock, flags);
	eie->played_frames = 0;
	eie->play_buf_pos = 0;
	substream->runtime->delay = 0;
	spin_unlock_irqrestore(&eie->lock, flags);

	return err;
}

static int eie_cpcm_prepare(struct snd_pcm_substream *substream)
{
	int err = 0;
	unsigned long flags;
	struct eie *eie = substream->private_data;

	dev_dbg(&eie->udev->dev, "cap: prepare rate=%u eie->rate=%u\n",
		substream->runtime->rate, READ_ONCE(eie->rate));
	mutex_lock(&eie->mutex);
	err = eie_prepare_rate(eie, substream->runtime->rate, PLAYBACK_RUNNING);
	mutex_unlock(&eie->mutex);

	spin_lock_irqsave(&eie->lock, flags);
	eie->cap_frames = 0;
	eie->cap_buf_pos = 0;
	substream->runtime->delay = 0; // TODO
	spin_unlock_irqrestore(&eie->lock, flags);

	return err;
}


static unsigned int calc_frames_wanted(struct eie *eie)
{
	eie->wanted_idx = 1 - eie->wanted_idx;
	if (eie->rate == 44100)
		return 220 + eie->wanted_idx;
	else
		return 5 * eie->rate / 1000;
}

/** Returns the number of filled frames or negative val for error */
static __must_check int fill_playback_urb(struct eie_playback_urb *epu)
{
	struct snd_pcm_runtime *runtime;
	struct eie *eie = epu->eie;
	struct urb *urb = epu->urb;

	unsigned int frames_wanted = calc_frames_wanted(eie);
	unsigned int frames_elapsed = atomic_xchg(&eie->frames_elapsed, 0);
	unsigned int frames_filled = 0;
	unsigned int bytes_wanted;
	unsigned char *start;

	int i;

	/* adjust frames_wanted by the frames_elapsed from EIE */
	if ((frames_elapsed > frames_wanted - 10)
		&& (frames_elapsed < frames_wanted + 10)) {
		frames_wanted = frames_elapsed;
	}
	bytes_wanted = BYTES_PER_FRAME * frames_wanted;

	if (bytes_wanted > urb->transfer_buffer_length)
		return -EINVAL;

	if (test_bit(PLAYBACK_RUNNING, &eie->states)) {
		runtime = eie->play_substream->runtime;

		if (frames_wanted > runtime->buffer_size)
			return -EINVAL;

		/* copy from ALSA's buffer to urb */
		start = runtime->dma_area + eie->play_buf_pos * BYTES_PER_FRAME;
		if (eie->play_buf_pos + frames_wanted <= runtime->buffer_size) {
			memcpy(urb->transfer_buffer, start, bytes_wanted);
			eie->play_buf_pos += frames_wanted;
		} else {
			unsigned int part_bytes = BYTES_PER_FRAME *
				(runtime->buffer_size - eie->play_buf_pos);
			memcpy(urb->transfer_buffer, start, part_bytes);
			memcpy(urb->transfer_buffer + part_bytes,
				runtime->dma_area,
				bytes_wanted - part_bytes);
			eie->play_buf_pos += frames_wanted;
		}
		eie->play_buf_pos %= runtime->buffer_size;
		eie->played_frames += frames_wanted;
		runtime->delay += frames_wanted;
		epu->silent = false;
		epu->len = frames_wanted;
	} else {
		if (!epu->silent && frames_wanted <= epu->len) {
			memset(urb->transfer_buffer, 0, bytes_wanted);
			epu->len = frames_wanted;
			epu->silent = true;
		}
	}

	/* adjust iso frame sizes */
	for (i = 0; i < PLAY_PKT_CNT; i++) {
		int len =  frames_wanted * (i+1) / PLAY_PKT_CNT - frames_filled;

		urb->iso_frame_desc[i].offset = frames_filled * BYTES_PER_FRAME;
		urb->iso_frame_desc[i].length = len * BYTES_PER_FRAME;
		frames_filled += len;
	}

	return 0;
}

static struct snd_pcm_substream *check_period_elapsed(struct eie *eie)
{
	struct snd_pcm_substream *substream = eie->play_substream;

	if (substream != NULL
		&& eie->played_frames >= substream->runtime->period_size) {
		eie->played_frames %= substream->runtime->period_size;
		return substream;
	}
	return NULL;
}

static int submit_init_play_urbs(struct eie *eie)
{
	unsigned long flags;
	int err;
	int i;

	spin_lock_irqsave(&eie->lock, flags);

	for (i = 0; i < PLAY_URB_CNT; i++) {
		/* init the urb state */
		eie->play_urbs[i].silent = true;
		eie->play_urbs[i].len = 0;

		err = fill_playback_urb(&eie->play_urbs[i]);
		if (err < 0)
			goto out;
		err = usb_submit_urb(eie->play_urbs[i].urb, GFP_ATOMIC);
		if (err < 0)
			goto out;
	}

out:
	spin_unlock_irqrestore(&eie->lock, flags);

	return err;
}

static int eie_ppcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct eie *eie = substream->private_data;
	unsigned long flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		dev_dbg(&eie->udev->dev, "play: SNDRV_PCM_TRIGGER_START");
		spin_lock_irqsave(&eie->lock, flags);
		set_bit(PLAYBACK_RUNNING, &eie->states);
		spin_unlock_irqrestore(&eie->lock, flags);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		dev_dbg(&eie->udev->dev, "play: SNDRV_PCM_TRIGGER_STOP");
		spin_lock_irqsave(&eie->lock, flags);
		clear_bit(PLAYBACK_RUNNING, &eie->states);
		spin_unlock_irqrestore(&eie->lock, flags);
		return 0;
	default:
		return -EINVAL;
	}
}

static int eie_cpcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct eie *eie = substream->private_data;
	unsigned long flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		dev_dbg(&eie->udev->dev, "cap: SNDRV_PCM_TRIGGER_START");
		spin_lock_irqsave(&eie->lock, flags);
		set_bit(CAPTURE_RUNNING, &eie->states);
		spin_unlock_irqrestore(&eie->lock, flags);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		dev_dbg(&eie->udev->dev, "cap: SNDRV_PCM_TRIGGER_STOP");
		spin_lock_irqsave(&eie->lock, flags);
		clear_bit(CAPTURE_RUNNING, &eie->states);
		spin_unlock_irqrestore(&eie->lock, flags);
		return 0;
	default:
		return -EINVAL;
	}
}


static snd_pcm_uframes_t eie_ppcm_pointer(struct snd_pcm_substream *substream)
{
	unsigned long flags;
	struct eie *eie = substream->private_data;
	snd_pcm_uframes_t pos;

	spin_lock_irqsave(&eie->lock, flags);
	pos = eie->play_buf_pos;
	spin_unlock_irqrestore(&eie->lock, flags);

	return pos;
}

static snd_pcm_uframes_t eie_cpcm_pointer(struct snd_pcm_substream *substream)
{
	unsigned long flags;
	struct eie *eie = substream->private_data;
	snd_pcm_uframes_t pos;

	spin_lock_irqsave(&eie->lock, flags);
	pos = eie->cap_buf_pos;
	spin_unlock_irqrestore(&eie->lock, flags);

	return pos;
}


static int eie_min_open(struct snd_rawmidi_substream *substream)
{
	int i, err;
	struct eie *eie = substream->rmidi->private_data;

	if (test_bit(MIN_OPEN, &eie->states))
		return -EINVAL;

	for (i = 0; i < MIN_URB_CNT; i++) {
		err = usb_submit_urb(eie->min_urbs[i], GFP_KERNEL);
		if (err < 0)
			goto err;
	}
	set_bit(MIN_OPEN, &eie->states);
	eie->min_substream = substream;
	return 0;

err:
	for (i = 0; i < MIN_URB_CNT; i++)
		usb_kill_urb(eie->min_urbs[i]);
	dev_dbg(&eie->udev->dev, "Urb problem: %s", usb_error_string(err));

	return err;
}

static int eie_min_close(struct snd_rawmidi_substream *substream)
{
	int i;
	struct eie *eie = substream->rmidi->private_data;

	dev_dbg(&eie->udev->dev, "Closing!");
	for (i = 0; i < MIN_URB_CNT; i++)
		usb_kill_urb(eie->min_urbs[i]);

	clear_bit(MIN_OPEN, &eie->states);
	clear_bit(MIN_UP, &eie->states);
	eie->min_substream = NULL;
	return 0;
}

static void eie_min_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct eie *eie = substream->rmidi->private_data;

	if (test_bit(MIN_OPEN, &eie->states)) {
		if (up)
			set_bit(MIN_UP, &eie->states);
		else
			clear_bit(MIN_UP, &eie->states);
	}
}

static int eie_mout_open(struct snd_rawmidi_substream *substream)
{
	return 0;
}

static int eie_mout_close(struct snd_rawmidi_substream *substream)
{
	int i;
	struct eie *eie = substream->rmidi->private_data;

	for (i = 0; i < MOUT_URB_CNT; i++)
		usb_kill_urb(eie->mout_urbs[i]);

	return 0;
}

static void eie_mout_trigger(struct snd_rawmidi_substream *substream, int up)
{
	int i, urb_idx, err;
	struct eie *eie = substream->rmidi->private_data;
	struct urb *urb = NULL;

	if (up <= 0)
		return;

	/* find free urb */
	for (i = 0; i < MOUT_URB_CNT; i++) {
		if (test_and_set_bit(i, &eie->submitted_mout_urbs) == 0) {
			urb = eie->mout_urbs[i];
			urb_idx = i;
			break;
		}
	}

	/* no free URB */
	if (urb == NULL)
		return;

	err = snd_rawmidi_transmit(substream, urb->transfer_buffer, 3);
	if (err <= 0) {
		clear_bit(urb_idx, &eie->submitted_mout_urbs);
		return;
	}
	for (i = err; i < 8; i++)
		((char *) urb->transfer_buffer)[i] = 0xfd;
	((char *) urb->transfer_buffer)[8] = 0xe0;
	urb->transfer_buffer_length = 9;

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		clear_bit(urb_idx, &eie->submitted_mout_urbs);
		dev_err(&eie->udev->dev, "Cannot submit midi-out urb.");
	}
}

/*
 * Called by the PCM core in process context after the stream was stopped
 * (also before prepare, hw_params and hw_free). Wait until no callback uses
 * the substream pointer it copied, so that close can free the runtime.
 */
static int eie_pcm_sync_stop(struct eie *eie, atomic_t *in_cb)
{
	if (!wait_event_timeout(eie->cb_wait, atomic_read(in_cb) == 0, HZ))
		dev_err(&eie->udev->dev, "Timeout waiting for URB callbacks.");
	return 0;
}

static int eie_ppcm_sync_stop(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;

	return eie_pcm_sync_stop(eie, &eie->play_in_cb);
}

static int eie_cpcm_sync_stop(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;

	return eie_pcm_sync_stop(eie, &eie->cap_in_cb);
}

static const struct snd_pcm_ops eie_playback_pcm_ops = {
	.open = eie_ppcm_open,
	.close = eie_ppcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = eie_pcm_hw_params,
	.hw_free = eie_pcm_hw_free,
	.prepare = eie_ppcm_prepare,
	.trigger = eie_ppcm_trigger,
	.sync_stop = eie_ppcm_sync_stop,
	.pointer = eie_ppcm_pointer,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	.page = snd_pcm_lib_get_vmalloc_page,
#else
	.page = eie_get_page,
#endif
};

static const struct snd_pcm_ops eie_capture_pcm_ops = {
	.open = eie_cpcm_open,
	.close = eie_cpcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = eie_pcm_hw_params,
	.hw_free = eie_pcm_hw_free,
	.prepare = eie_cpcm_prepare,
	.trigger = eie_cpcm_trigger,
	.sync_stop = eie_cpcm_sync_stop,
	.pointer = eie_cpcm_pointer,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	.page = snd_pcm_lib_get_vmalloc_page,
#else
	.page = eie_get_page,
#endif
};

static const struct snd_rawmidi_ops eie_midi_out_ops = {
	.open = eie_mout_open,
	.close = eie_mout_close,
	.trigger = eie_mout_trigger,
};

static const struct snd_rawmidi_ops eie_midi_in_ops = {
	.open = eie_min_open,
	.close = eie_min_close,
	.trigger = eie_min_trigger,
};

static void abort_playback(struct eie *eie)
{
	struct snd_pcm_substream *play = NULL, *cap = NULL;
	unsigned long flags;

	/* the stream lock must not be taken under eie->lock, so copy first */
	spin_lock_irqsave(&eie->lock, flags);
	if (test_bit(PLAYBACK_RUNNING, &eie->states)) {
		play = eie->play_substream;
		if (play)
			atomic_inc(&eie->play_in_cb);
	}
	if (test_bit(CAPTURE_RUNNING, &eie->states)) {
		cap = eie->cap_substream;
		if (cap)
			atomic_inc(&eie->cap_in_cb);
	}
	eie->rate = 0;
	spin_unlock_irqrestore(&eie->lock, flags);

	if (play) {
		snd_pcm_stream_lock_irqsave(play, flags);
		snd_pcm_stop(play, SNDRV_PCM_STATE_XRUN);
		snd_pcm_stream_unlock_irqrestore(play, flags);
		if (atomic_dec_and_test(&eie->play_in_cb))
			wake_up(&eie->cb_wait);
	}

	if (cap) {
		snd_pcm_stream_lock_irqsave(cap, flags);
		snd_pcm_stop(cap, SNDRV_PCM_STATE_XRUN);
		snd_pcm_stream_unlock_irqrestore(cap, flags);
		if (atomic_dec_and_test(&eie->cap_in_cb))
			wake_up(&eie->cb_wait);
	}
}

static void play_urb_complete(struct urb *urb)
{
	struct eie_playback_urb *epu = urb->context;
	struct eie *eie = epu->eie;
	unsigned long flags;
	int err;
	struct snd_pcm_substream *elapsed = NULL;
	bool abort = false;

	/* for ISO this means that we have been killed or unlinked */
	if (urb->status != 0) {
		dev_dbg(&eie->udev->dev, "Play urb complete. %d", urb->status);
		return;
	}

	/* first URB */
	if (!test_and_set_bit(URBS_FLOWING, &eie->states))
		wake_up(&eie->urbs_flow_wait);

	spin_lock_irqsave(&eie->lock, flags);
	if (!epu->silent
		&& eie->play_substream && eie->play_substream->runtime)
		eie->play_substream->runtime->delay -= epu->len;

	err = fill_playback_urb(epu);
	if (err < 0) {
		abort = true;
		goto err;
	}

	if (test_bit(PLAYBACK_RUNNING, &eie->states)) {
		elapsed = check_period_elapsed(eie);
		if (elapsed)
			atomic_inc(&eie->play_in_cb);
	}

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		dev_err(&eie->udev->dev, "Cannot resubmit play urb.");
		abort = true;
	}
err:
	spin_unlock_irqrestore(&eie->lock, flags);
	if (elapsed) {
		snd_pcm_period_elapsed(elapsed);
		if (atomic_dec_and_test(&eie->play_in_cb))
			wake_up(&eie->cb_wait);
	}
	if (abort)
		abort_playback(eie);
}

static void sync_urb_complete(struct urb *urb)
{
	struct eie *eie = urb->context;
	int i;
	int err;

	/* for ISO this means that we have been killed or unlinked */
	if (urb->status != 0) {
		dev_dbg(&eie->udev->dev, "Sync urb complete. status = %d, packets = %d",
			urb->status, urb->number_of_packets);
		return;
	}

	for (i = 0; i < urb->number_of_packets; i++) {
		if (urb->iso_frame_desc[i].actual_length > 0) {
			unsigned char *buf = urb->transfer_buffer;
			unsigned int offset = urb->iso_frame_desc[i].offset;
			unsigned char d = buf[offset];

			if (d == 0) {
				/* the device did not advance clock */
				dev_info_ratelimited(&eie->udev->dev,
						     "Sync urb: device did not advance clock\n");
				abort_playback(eie);
			}
			atomic_add(d, &eie->frames_elapsed);
		}
	}

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0)
		abort_playback(eie);
}

static void cap_urb_complete(struct urb *urb)
{
	struct eie *eie = urb->context;
	struct snd_pcm_substream *elapsed = NULL;
	const char *fault = NULL;
	unsigned int len;
	unsigned long flags;
	bool abort = false;
	bool resubmit_stopped = false;
	int err;

	if (urb->status != 0) {
		dev_dbg(&eie->udev->dev, "Capture urb complete. %d", urb->status);
		return;
	}

	/* the urb belongs to the HCD again after the resubmit below */
	len = urb->actual_length;

	spin_lock_irqsave(&eie->lock, flags);
	if (test_bit(CAPTURE_RUNNING, &eie->states) && eie->cap_substream) {
		struct snd_pcm_runtime *runtime = eie->cap_substream->runtime;
		unsigned char *buf = urb->transfer_buffer;
		unsigned int frames_rcvd = len / 64;
		unsigned int ch1, ch2, ch3, ch4;
		unsigned int i, j;

		if (!runtime->dma_area)
			fault = "no dma area";
		else if ((size_t)runtime->buffer_size * BYTES_PER_FRAME > runtime->dma_bytes)
			fault = "buffer larger than dma area";
		else if (eie->cap_buf_pos >= runtime->buffer_size)
			fault = "position outside buffer";
		else if (frames_rcvd > runtime->buffer_size)
			fault = "urb larger than buffer";

		if (fault) {
			abort = true;
		} else {
			for (i = 0; i < frames_rcvd; i++) {
				unsigned char *out = runtime->dma_area +
					eie->cap_buf_pos * BYTES_PER_FRAME;

				ch1 = ch2 = ch3 = ch4 = 0;
				for (j = 0; j < 24; j++) {
					ch1 |= (buf[64*i + j +  0]        & 1) << (23-j);
					ch2 |= (buf[64*i + j + 32]        & 1) << (23-j);
					ch3 |= ((buf[64*i + j +  0] >> 1) & 1) << (23-j);
					ch4 |= ((buf[64*i + j + 32] >> 1) & 1) << (23-j);
				}
				/* S24_3LE */
				out[0] = ch1;
				out[1] = ch1 >> 8;
				out[2] = ch1 >> 16;
				out[3] = ch2;
				out[4] = ch2 >> 8;
				out[5] = ch2 >> 16;
				out[6] = ch3;
				out[7] = ch3 >> 8;
				out[8] = ch3 >> 16;
				out[9] = ch4;
				out[10] = ch4 >> 8;
				out[11] = ch4 >> 16;

				if (++eie->cap_buf_pos == runtime->buffer_size)
					eie->cap_buf_pos = 0;
			}

			/*
			 * One urb carries at most CAP_BUF_SIZE / 64 frames (64 bytes
			 * per frame on the wire), i.e. 64 frames. period_bytes_min is
			 * 64 * BYTES_PER_FRAME, so period_size is at least 64 frames
			 * and one period_elapsed per urb is enough.
			 */
			eie->cap_frames += frames_rcvd;
			if (eie->cap_frames >= runtime->period_size) {
				eie->cap_frames %= runtime->period_size;
				elapsed = eie->cap_substream;
				atomic_inc(&eie->cap_in_cb);
			}
		}
	}

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		/* urb being killed or device gone: not a capture fault */
		if (err == -EPERM || err == -ENODEV || err == -ESHUTDOWN) {
			resubmit_stopped = true;
		} else {
			if (!fault)
				fault = "cannot resubmit urb";
			abort = true;
		}
	}
	spin_unlock_irqrestore(&eie->lock, flags);

	if (elapsed) {
		snd_pcm_period_elapsed(elapsed);
		if (atomic_dec_and_test(&eie->cap_in_cb))
			wake_up(&eie->cb_wait);
	}
	if (resubmit_stopped)
		dev_dbg(&eie->udev->dev, "Capture urb not resubmitted: %d", err);
	dev_dbg_ratelimited(&eie->udev->dev, "Capture urb length %u", len);
	if (len % 64)
		dev_warn_ratelimited(&eie->udev->dev,
			"Capture urb length %u is not a multiple of 64\n", len);
	if (abort) {
		dev_err_ratelimited(&eie->udev->dev, "Capture aborted: %s\n",
			fault);
		abort_playback(eie);
	}
}

static void min_urb_complete(struct urb *urb)
{
	struct eie *eie = urb->context;
	int i, err;

	if (urb->status != 0) {
		dev_dbg(&eie->udev->dev, "Midi in urb complete. %d", urb->status);
		return;
	}

	if (test_bit(MIN_UP, &eie->states))
		for (i = 0; i < urb->actual_length; i++) {
			__u8 b = ((__u8 *)urb->transfer_buffer)[i];

			if (b != 0xfd)
				snd_rawmidi_receive(eie->min_substream, &b, 1);
		}

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0)
		dev_err(&eie->udev->dev, "Cannot resubmit midi-in urb.");
}

static void mout_urb_complete(struct urb *urb)
{
	struct eie *eie = urb->context;
	int i;

	if (urb->status != 0)
		dev_dbg(&eie->udev->dev, "Midi out urb complete. %d", urb->status);

	for (i = 0; i < MOUT_URB_CNT; i++) {
		if (eie->mout_urbs[i] == urb) {
			clear_bit(i, &eie->submitted_mout_urbs);
			break;
		}
	}
}

static void kill_all_urbs(struct eie *eie)
{
	struct urb *urb;
	int i;

	for (i = 0; i < PLAY_URB_CNT; i++) {
		urb = eie->play_urbs[i].urb;
		if (urb)
			usb_kill_urb(urb);
	}

	for (i = 0; i < SYNC_URB_CNT; i++) {
		urb = eie->sync_urbs[i];
		if (urb)
			usb_kill_urb(urb);
	}

	for (i = 0; i < CAP_URB_CNT; i++) {
		urb = eie->cap_urbs[i];
		if (urb)
			usb_kill_urb(urb);
	}
}

/* size must be the size passed to usb_alloc_coherent(), not the URB length */
static void kill_and_free_urb(struct eie *eie, struct urb **urbp, size_t size)
{
	struct urb *urb = *urbp;

	if (urb) {
		usb_kill_urb(urb);
		if (urb->transfer_buffer != NULL)
			usb_free_coherent(eie->udev, size,
				urb->transfer_buffer,
				urb->transfer_dma);
		usb_free_urb(urb);
		(*urbp) = NULL;
	}
}

static void free_usb_related_resources(struct eie *eie)
{
	int i;

	for (i = 0; i < PLAY_URB_CNT; i++)
		kill_and_free_urb(eie, &eie->play_urbs[i].urb,
				 PLAY_PKT_CNT * eie->play_packet_size);

	for (i = 0; i < SYNC_URB_CNT; i++)
		kill_and_free_urb(eie, &eie->sync_urbs[i],
				 eie->sync_packet_size);

	for (i = 0; i < CAP_URB_CNT; i++)
		kill_and_free_urb(eie, &eie->cap_urbs[i], eie->cap_buf_size);

	for (i = 0; i < MIN_URB_CNT; i++)
		kill_and_free_urb(eie, &eie->min_urbs[i], eie->min_buf_size);

	for (i = 0; i < MOUT_URB_CNT; i++)
		kill_and_free_urb(eie, &eie->mout_urbs[i], eie->mout_buf_size);

	if (eie->ifb) {
		usb_set_intfdata(eie->ifb, NULL);
		usb_driver_release_interface(&eie_driver, eie->ifb);
	}

	if (eie->ifa)
		usb_set_intfdata(eie->ifa, NULL);
}


static int init_play_urbs(struct eie *eie,
	struct usb_endpoint_descriptor *endpoint)
{
	unsigned char *buf;
	struct urb *urb;
	int j;
	int err = 0;

	eie->play_packet_size = usb_endpoint_maxp(endpoint);
	eie->play_endpoint_addr = endpoint->bEndpointAddress;

	for (j = 0; j < PLAY_URB_CNT; j++) {
		urb = usb_alloc_urb(PLAY_PKT_CNT, GFP_KERNEL);
		if (urb == NULL) {
			err = -ENOMEM;
			break;
		}

		buf = usb_alloc_coherent(eie->udev,
			PLAY_PKT_CNT * eie->play_packet_size,
			GFP_KERNEL, &urb->transfer_dma);
		if (buf == NULL) {
			usb_free_urb(urb);
			err = -ENOMEM;
			break;
		}

		urb->dev = eie->udev;
		urb->pipe = usb_sndisocpipe(eie->udev, eie->play_endpoint_addr);
		urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
		urb->transfer_buffer = buf;
		/* urb->transfer_dma - set from usb_alloc_coherent */
		urb->transfer_buffer_length = PLAY_PKT_CNT * eie->play_packet_size;
		urb->number_of_packets = PLAY_PKT_CNT;
		urb->interval = 1;
		urb->context = &eie->play_urbs[j];
		urb->complete = play_urb_complete;

		eie->play_urbs[j].urb = urb;
		eie->play_urbs[j].eie = eie;
	}

	return err;
}

static int init_sync_urbs(struct eie *eie,
	struct usb_endpoint_descriptor *endpoint)
{
	unsigned char *buf;
	struct urb *urb;
	int j;
	int err = 0;

	eie->sync_packet_size = usb_endpoint_maxp(endpoint);
	eie->sync_endpoint_addr = endpoint->bEndpointAddress;

	for (j = 0; j < SYNC_URB_CNT; j++) {
		urb = usb_alloc_urb(1, GFP_KERNEL);
		if (urb == NULL) {
			err = -ENOMEM;
			break;
		}

		buf = usb_alloc_coherent(eie->udev, eie->sync_packet_size,
			GFP_KERNEL, &urb->transfer_dma);
		if (buf == NULL) {
			usb_free_urb(urb);
			err = -ENOMEM;
			break;
		}

		urb->dev = eie->udev;
		urb->pipe = usb_rcvisocpipe(eie->udev, eie->sync_endpoint_addr);
		urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
		urb->transfer_buffer = buf;
		/* urb->transfer_dma - set from usb_alloc_coherent */
		urb->transfer_buffer_length = eie->sync_packet_size;
		urb->number_of_packets = 1;
		urb->interval = 1;
		urb->context = eie;
		urb->complete = sync_urb_complete;
		urb->iso_frame_desc[0].offset = 0;
		urb->iso_frame_desc[0].length = eie->sync_packet_size;

		eie->sync_urbs[j] = urb;
	}

	return err;
}


static int init_cap_urbs(struct eie *eie,
	struct usb_endpoint_descriptor *endpoint)
{
	unsigned char *buf;
	struct urb *urb;
	int j;
	int err = 0;

	eie->cap_endpoint_addr = endpoint->bEndpointAddress;
	if (!usb_endpoint_maxp(endpoint) ||
	    CAP_BUF_SIZE % usb_endpoint_maxp(endpoint)) {
		dev_err(&eie->udev->dev, "Capture buffer size %d does not fit endpoint packet size %d\n",
			CAP_BUF_SIZE, usb_endpoint_maxp(endpoint));
		return -EINVAL;
	}
	eie->cap_buf_size = CAP_BUF_SIZE;
	for (j = 0; j < CAP_URB_CNT; j++) {
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (urb == NULL) {
			err = -ENOMEM;
			break;
		}

		buf = usb_alloc_coherent(eie->udev, eie->cap_buf_size,
			GFP_KERNEL, &urb->transfer_dma);
		if (buf == NULL) {
			usb_free_urb(urb);
			err = -ENOMEM;
			break;
		}

		usb_fill_bulk_urb(urb, eie->udev,
			usb_rcvbulkpipe(eie->udev, eie->cap_endpoint_addr), buf,
			eie->cap_buf_size, cap_urb_complete, eie);
		urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;

		eie->cap_urbs[j] = urb;
	}

	return err;
}

static int init_mout_urbs(struct eie *eie,
	struct usb_endpoint_descriptor *endpoint)
{
	unsigned char *buf;
	struct urb *urb;
	int j;
	int err = 0;

	eie->mout_endpoint_addr = endpoint->bEndpointAddress;
	eie->mout_buf_size = usb_endpoint_maxp(endpoint);
	for (j = 0; j < MOUT_URB_CNT; j++) {
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (urb == NULL) {
			err = -ENOMEM;
			break;
		}

		buf = usb_alloc_coherent(eie->udev, usb_endpoint_maxp(endpoint),
			GFP_KERNEL, &urb->transfer_dma);
		if (buf == NULL) {
			usb_free_urb(urb);
			err = -ENOMEM;
			break;
		}

		usb_fill_bulk_urb(urb, eie->udev,
			usb_sndbulkpipe(eie->udev, eie->mout_endpoint_addr), buf,
			usb_endpoint_maxp(endpoint), mout_urb_complete, eie);
		urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;

		eie->mout_urbs[j] = urb;
	}

	return err;
}

static int init_min_urbs(struct eie *eie,
	struct usb_endpoint_descriptor *endpoint)
{
	unsigned char *buf;
	struct urb *urb;
	int j;
	int err = 0;

	eie->min_endpoint_addr = endpoint->bEndpointAddress;
	eie->min_buf_size = usb_endpoint_maxp(endpoint);
	for (j = 0; j < MIN_URB_CNT; j++) {
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (urb == NULL) {
			err = -ENOMEM;
			break;
		}

		buf = usb_alloc_coherent(eie->udev, usb_endpoint_maxp(endpoint),
			GFP_KERNEL, &urb->transfer_dma);
		if (buf == NULL) {
			usb_free_urb(urb);
			err = -ENOMEM;
			break;
		}

		usb_fill_bulk_urb(urb, eie->udev,
			usb_rcvbulkpipe(eie->udev, eie->min_endpoint_addr), buf,
			usb_endpoint_maxp(endpoint), min_urb_complete, eie);
		urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;

		eie->min_urbs[j] = urb;
	}

	return err;
}

static int init_urbs(struct eie *eie)
{
	int i, err;

	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;

	iface_desc = eie->ifa->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (!eie->play_endpoint_addr && usb_endpoint_is_isoc_out(endpoint)) {
			err = init_play_urbs(eie, endpoint);
			if (err < 0)
				return err;
		}
		if (!eie->mout_endpoint_addr && usb_endpoint_is_bulk_out(endpoint)) {
			err = init_mout_urbs(eie, endpoint);
			if (err < 0)
				return err;
		}
		if (!eie->min_endpoint_addr && usb_endpoint_is_bulk_in(endpoint)) {
			err = init_min_urbs(eie, endpoint);
			if (err < 0)
				return err;
		}
	}

	iface_desc = eie->ifb->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (!eie->sync_endpoint_addr && usb_endpoint_is_isoc_in(endpoint)) {
			err = init_sync_urbs(eie, endpoint);
			if (err < 0)
				return err;
		}

		if (!eie->cap_endpoint_addr && usb_endpoint_is_bulk_in(endpoint)) {
			err = init_cap_urbs(eie, endpoint);
			if (err < 0)
				return err;
		}
	}

	if (!(eie->cap_endpoint_addr && eie->play_endpoint_addr
		&& eie->mout_endpoint_addr && eie->min_endpoint_addr
		&& eie->sync_endpoint_addr)) {
		dev_err(&eie->udev->dev, "Cannot find expected endpoints.");
		return -ENOENT;
	}

	return 0;
}

static int eie_probe(struct usb_interface *interface,
	const struct usb_device_id *usb_id)
{
	char *name = "EIE pro";

	unsigned int card_index;
	struct snd_card *card;
	struct snd_rawmidi *rmidi;
	struct eie *eie;

	char usb_path[32];

	int err;

	mutex_lock(&devices_mutex);

	for (card_index = 0; card_index < SNDRV_CARDS; ++card_index)
		if (enable[card_index] && !(devices_used & (1 << card_index)))
			break;
	if (card_index >= SNDRV_CARDS) {
		mutex_unlock(&devices_mutex);
		return -ENOENT;
	}
	err = snd_card_new(&interface_to_usbdev(interface)->dev, index[card_index], id[card_index], THIS_MODULE,
			      sizeof(*eie), &card);
	if (err < 0) {
		mutex_unlock(&devices_mutex);
		return err;
	}

	/* card->private_free = eie_card_free; */
	eie = card->private_data;
	eie->udev = interface_to_usbdev(interface);
	eie->card = card;
	eie->card_index = card_index;

	mutex_init(&eie->mutex);
	spin_lock_init(&eie->lock);
	init_waitqueue_head(&eie->urbs_flow_wait);
	init_waitqueue_head(&eie->cb_wait);
	atomic_set(&eie->play_in_cb, 0);
	atomic_set(&eie->cap_in_cb, 0);

	eie->ifa = interface;
	eie->ifb = usb_ifnum_to_if(eie->udev, 1);
	if (!eie->ifb) {
		err = -ENXIO;
		goto probe_err;
	}
	err = usb_driver_claim_interface(&eie_driver, eie->ifb, eie);
	if (err < 0) {
		eie->ifb = NULL;
		err = -EBUSY;
		goto probe_err;
	}

	err = eie_set_alt_setting(eie);
	if (err < 0)
		goto probe_err;


	/* prepare the card struct */
	snd_card_set_dev(card, &interface->dev);

	strscpy(card->driver, "EIE");
	strscpy(card->shortname, name);
	usb_make_path(eie->udev, usb_path, sizeof(usb_path));
	snprintf(card->longname, sizeof(card->longname),
		 "Akai EIE pro, at %s, %s speed", usb_path,
		 eie->udev->speed == USB_SPEED_HIGH ? "high" : "full");

	err = snd_pcm_new(card, name, 0, 1, 1, &eie->pcm);
	if (err < 0)
		goto probe_err;
	eie->pcm->private_data = eie;
	strscpy(eie->pcm->name, name);
	snd_pcm_set_ops(eie->pcm, SNDRV_PCM_STREAM_PLAYBACK, &eie_playback_pcm_ops);
	snd_pcm_set_ops(eie->pcm, SNDRV_PCM_STREAM_CAPTURE, &eie_capture_pcm_ops);

	err = snd_rawmidi_new(card, "eiepro", 0, 1, 1, &rmidi);
	if (err < 0)
		goto probe_err;
	rmidi->private_data = eie;
	strscpy(rmidi->name, "EIE pro");
	rmidi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT |
	SNDRV_RAWMIDI_INFO_INPUT |
	SNDRV_RAWMIDI_INFO_DUPLEX;

	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &eie_midi_out_ops);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &eie_midi_in_ops);

	eie->rmidi = rmidi;

	err = init_urbs(eie);
	if (err < 0)
		goto probe_err;

	err = snd_card_register(card);
	if (err < 0)
		goto probe_err;

	usb_set_intfdata(interface, eie);
	devices_used |= 1 << card_index;

	mutex_unlock(&devices_mutex);
	return 0;

probe_err:
	/* no new user of the card from here on; see eie_disconnect() */
	set_bit(DISCONNECTED, &eie->states);
	wake_up(&eie->urbs_flow_wait);
	snd_card_disconnect(card);

	mutex_lock(&eie->mutex);
	free_usb_related_resources(eie);
	mutex_unlock(&eie->mutex);
	snd_card_free(card);
	mutex_unlock(&devices_mutex);
	return err;
}

static void eie_disconnect(struct usb_interface *interface)
{
	struct eie *eie = usb_get_intfdata(interface);
	unsigned int card_index;

	if (!eie)
		return;

	mutex_lock(&devices_mutex);

	/* eie may be freed by snd_card_free_when_closed() below */
	card_index = eie->card_index;

	set_bit(DISCONNECTED, &eie->states);
	/* release a reset_eie() waiting for URBs while it holds eie->mutex */
	wake_up(&eie->urbs_flow_wait);

	/*
	 * Stop user space (no new open/ioctl, running PCMs are stopped)
	 * before the URBs are freed.
	 */
	snd_card_disconnect(eie->card);

	mutex_lock(&eie->mutex);
	free_usb_related_resources(eie);
	mutex_unlock(&eie->mutex);

	/*
	 * As in ua101: the index is released before the card is actually
	 * freed, which may be delayed until the last file is closed.
	 */
	devices_used &= ~(1U << card_index);

	snd_card_free_when_closed(eie->card);

	mutex_unlock(&devices_mutex);
}


static struct usb_device_id eie_ids[] = {
	{ USB_DEVICE(0x09e8, 0x0010) }, /* EIE pro */
	{ }
};
MODULE_DEVICE_TABLE(usb, eie_ids);

static struct usb_driver eie_driver = {
	.name = "snd-eie",
	.id_table = eie_ids,
	.probe = eie_probe,
	.disconnect = eie_disconnect,
	// .suspend = eie_suspend,
	// .resume = eie_resume,
};

module_usb_driver(eie_driver);
