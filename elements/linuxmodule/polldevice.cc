/*
 * polldevice.{cc,hh} -- element steals packets from Linux devices by polling.
 * Benjie Chen
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 * Copyright (c) 2000 Mazu Networks, Inc.
 * Copyright (c) 2001 International Computer Science Institute
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include <click/glue.hh>
#include "polldevice.hh"
#include "fromdevice.hh"
#include "todevice.hh"
#include <click/error.hh>
#include <click/confparse.hh>
#include <click/router.hh>
#include <click/skbmgr.hh>
#include "elements/standard/scheduleinfo.hh"

#include <click/cxxprotect.h>
CLICK_CXX_PROTECT
#include <linux/netdevice.h>
#include <linux/sched.h>
#include <asm/msr.h>
CLICK_CXX_UNPROTECT
#include <click/cxxunprotect.h>

/* for hot-swapping */
static AnyDeviceMap poll_device_map;
static int poll_device_count;
static struct notifier_block device_notifier;
extern "C" {
static int device_notifier_hook(struct notifier_block *nb, unsigned long val, void *v);
}

static void
polldev_static_initialize()
{
    if (++poll_device_count == 1) {
	poll_device_map.initialize();
	device_notifier.notifier_call = device_notifier_hook;
	device_notifier.priority = 1;
	device_notifier.next = 0;
	register_netdevice_notifier(&device_notifier);
    }
}

static void
polldev_static_cleanup()
{
    if (--poll_device_count <= 0)
	unregister_netdevice_notifier(&device_notifier);
}

PollDevice::PollDevice()
{
  // no MOD_INC_USE_COUNT; rely on AnyDevice
  add_output();
  polldev_static_initialize();
}

PollDevice::~PollDevice()
{
  // no MOD_DEC_USE_COUNT; rely on AnyDevice
  polldev_static_cleanup();
}


int
PollDevice::configure(const Vector<String> &conf, ErrorHandler *errh)
{
  _burst = 8;
  _promisc = false;
  bool allow_nonexistent = false;
  if (cp_va_parse(conf, this, errh,
		  cpString, "interface name", &_devname,
		  cpOptional,
		  cpBool, "enter promiscuous mode?", &_promisc,
		  cpUnsigned, "burst size", &_burst,
		  cpKeywords,
		  "PROMISC", cpBool, "enter promiscuous mode?", &_promisc,
		  "PROMISCUOUS", cpBool, "enter promiscuous mode?", &_promisc,
		  "BURST", cpUnsigned, "burst size", &_burst,
		  "ALLOW_NONEXISTENT", cpBool, "allow nonexistent interface?", &allow_nonexistent,
		  cpEnd) < 0)
    return -1;
  
#if HAVE_POLLING
  if (find_device(allow_nonexistent, errh) < 0)
      return -1;
  // must check both _dev->polling and _dev->poll_on as some drivers
  // memset() their device structures to all zero
  if (_dev && (_dev->polling < 0 || !_dev->poll_on))
      return errh->error("device `%s' not pollable, use FromDevice instead", _devname.cc());
#endif
  
  return 0;
}


/*
 * Use Linux interface for polling, added by us, in include/linux/netdevice.h,
 * to poll devices.
 */
int
PollDevice::initialize(ErrorHandler *errh)
{
#if HAVE_POLLING
  /* try to find a ToDevice with the same device: if none exists, then we need
   * to manage tx queue as well as rx queue. need to do it this way because
   * ToDevice may not have been initialized
   */
  if (_dev)
      for (int fi = 0; fi < router()->nelements(); fi++) {
	  Element *e = router()->element(fi);
	  if (e == this) continue;
	  if (PollDevice *pd=(PollDevice *)(e->cast("PollDevice"))) {
	      if (pd->ifindex() == ifindex())
		  return errh->error("duplicate PollDevice for `%s'", _devname.cc());
	  } else if (FromDevice *fd = (FromDevice *)(e->cast("FromDevice"))) {
	      if (fd->ifindex() == ifindex())
		  return errh->error("both FromDevice and PollDevice for `%s'", _devname.cc());
	  }
      }
  
  poll_device_map.insert(this);
  if (_dev && _promisc)
      dev_set_promiscuity(_dev, 1);
  if (_dev && !_dev->polling) {
      /* turn off interrupt if interrupts weren't already off */
      _dev->poll_on(_dev);
      if (_dev->polling != 2)
	  return errh->error("PollDevice detected wrong version of polling patch");
  }
  
  ScheduleInfo::initialize_task(this, &_task, _dev != 0, errh);
#ifdef HAVE_STRIDE_SCHED
  // user specifies max number of tickets; we start with default
  _max_tickets = _task.tickets();
  _task.set_tickets(Task::DEFAULT_TICKETS);
#endif

  reset_counts();
  return 0;
#else
  errh->warning("can't get packets: not compiled with polling extensions");
  return 0;
#endif
}

void
PollDevice::reset_counts()
{
  _npackets = 0;

#if CLICK_DEVICE_STATS
  _activations = 0;
  _empty_polls = 0;
  _time_poll = 0;
  _time_refill = 0;
  _time_allocskb = 0;
  _perfcnt1_poll = 0;
  _perfcnt1_refill = 0;
  _perfcnt1_allocskb = 0;
  _perfcnt1_pushing = 0;
  _perfcnt2_poll = 0;
  _perfcnt2_refill = 0;
  _perfcnt2_allocskb = 0;
  _perfcnt2_pushing = 0;
#endif
#if CLICK_DEVICE_THESIS_STATS || CLICK_DEVICE_STATS
  _push_cycles = 0;
#endif
}

void
PollDevice::uninitialize()
{
#if HAVE_POLLING
  poll_device_map.remove(this);
  if (poll_device_map.lookup(_dev) == 0) {
    if (_dev && _dev->polling > 0)
      _dev->poll_off(_dev);
  }
  if (_dev && _promisc)
      dev_set_promiscuity(_dev, -1);
#if LINUX_VERSION_CODE >= 0x020400
  if (_dev)
    dev_put(_dev);
#endif
  _task.unschedule();
#endif
}

void
PollDevice::run_scheduled()
{
#if HAVE_POLLING
  struct sk_buff *skb_list, *skb;
  int got=0;
#if CLICK_DEVICE_STATS
  unsigned long long time_now;
  unsigned low00, low10;
#endif

  SET_STATS(low00, low10, time_now);

  got = _burst;
  skb_list = _dev->rx_poll(_dev, &got);

#if CLICK_DEVICE_STATS
  if (got > 0 || _activations > 0) {
    GET_STATS_RESET(low00, low10, time_now, 
		    _perfcnt1_poll, _perfcnt2_poll, _time_poll);
    if (got == 0) 
      _empty_polls++;
    else 
      _activations++;
  }
#endif

  int nskbs = got;
  if (got == 0)
    nskbs = _dev->rx_refill(_dev, 0);

  if (nskbs > 0) {
    /*
     * Extra 16 bytes in the SKB for eepro100 RxFD -- perhaps there
     * should be some callback to the device driver to query for the
     * desired packet size.
     */
    struct sk_buff *new_skbs = skbmgr_allocate_skbs(0, 1536+16, &nskbs);

#if CLICK_DEVICE_STATS
    if (_activations > 0)
      GET_STATS_RESET(low00, low10, time_now, 
	              _perfcnt1_allocskb, _perfcnt2_allocskb, _time_allocskb);
#endif

    nskbs = _dev->rx_refill(_dev, &new_skbs);

#if CLICK_DEVICE_STATS
    if (_activations > 0) 
      GET_STATS_RESET(low00, low10, time_now, 
	              _perfcnt1_refill, _perfcnt2_refill, _time_refill);
#endif

    if (new_skbs) {
      click_chatter("too much skbs for refill");
      skbmgr_recycle_skbs(new_skbs, 0);
    }
  }

  for (int i = 0; i < got; i++) {
    skb = skb_list;
    skb_list = skb_list->next;
    skb->next = NULL;
 
    if (skb_list) {
      // prefetch annotation area, and first 2 cache
      // lines that contain ethernet and ip headers.
#if __i386__ && HAVE_INTEL_CPU
      asm volatile("prefetcht0 %0" : : "m" (skb_list->cb[0]));
      // asm volatile("prefetcht0 %0" : : "m" (*(skb_list->data)));
      asm volatile("prefetcht0 %0" : : "m" (*(skb_list->data+32)));
#endif
    }

    /* Retrieve the ether header. */
    skb_push(skb, 14);
    if (skb->pkt_type == PACKET_HOST)
      skb->pkt_type |= PACKET_CLEAN;

    Packet *p = Packet::make(skb); 
   
#ifndef CLICK_WARP9
    struct timeval &tv = p->timestamp_anno(); 
    click_gettimeofday(&tv);
#endif

    _npackets++;
#if CLICK_DEVICE_THESIS_STATS && !CLICK_DEVICE_STATS
    unsigned long long before_push_cycles = click_get_cycles();
#endif
    output(0).push(p);
#if CLICK_DEVICE_THESIS_STATS && !CLICK_DEVICE_STATS
    _push_cycles += click_get_cycles() - before_push_cycles - CLICK_CYCLE_COMPENSATION;
#endif
  }

#if CLICK_DEVICE_STATS
  if (_activations > 0) {
    GET_STATS_RESET(low00, low10, time_now, 
	            _perfcnt1_pushing, _perfcnt2_pushing, _push_cycles);
#if _DEV_OVRN_STATS_
    if ((_activations % 1024) == 0) _dev->get_stats(_dev);
#endif
  }
#endif

  adjust_tickets(got);
  _task.fast_reschedule();

#endif /* HAVE_POLLING */
}

void
PollDevice::change_device(net_device *dev)
{
#if HAVE_POLLING
    _task.unschedule();
    
    if (dev && (dev->polling < 0 || !dev->poll_on)) {
	click_chatter("%s: device `%s' does not support polling", declaration().cc(), _devname.cc());
	dev = 0;
    }
    
    if (!_dev && dev)
	click_chatter("%s: device `%s' came up", declaration().cc(), _devname.cc());
    else if (_dev && !dev)
	click_chatter("%s: device `%s' went down", declaration().cc(), _devname.cc());
    
    poll_device_map.remove(this);
    if (_dev)
	_dev->poll_off(_dev);
    if (_dev && _promisc)
	dev_set_promiscuity(_dev, -1);
#if LINUX_VERSION_CODE >= 0x020400
    if (_dev)
	dev_put(_dev);
#endif
    
    _dev = dev;
#if LINUX_VERSION_CODE >= 0x020400
    if (_dev)
	dev_hold(_dev);
#endif
    if (_dev && !_dev->polling)
	_dev->poll_on(_dev);
    if (_dev && _promisc)
	dev_set_promiscuity(_dev, 1);
    poll_device_map.insert(this);

    if (_dev)
	_task.reschedule();
#else
    (void) dev;
#endif /* HAVE_POLLING */
}

extern "C" {
static int
device_notifier_hook(struct notifier_block *nb, unsigned long flags, void *v)
{
    net_device *dev = (net_device *)v;

    if (flags == NETDEV_UP) {
	if (PollDevice *pd = (PollDevice *)poll_device_map.lookup_unknown(dev))
	    pd->change_device(dev);
    } else if (flags == NETDEV_DOWN) {
	if (PollDevice *pd = (PollDevice *)poll_device_map.lookup(dev))
	    pd->change_device(0);
    }

    return 0;
}
}

static String
PollDevice_read_calls(Element *f, void *)
{
  PollDevice *kw = (PollDevice *)f;
  return
    String(kw->_npackets) + " packets received\n" +
#if CLICK_DEVICE_STATS
    String(kw->_time_poll) + " cycles poll\n" +
    String(kw->_time_refill) + " cycles refill\n" +
    String(kw->_time_allocskb) + " cycles allocskb\n" +
    String(kw->_push_cycles) + " cycles pushing\n" +
    String(kw->_perfcnt1_poll) + " perfctr1 poll\n" +
    String(kw->_perfcnt1_refill) + " perfctr1 refill\n" +
    String(kw->_perfcnt1_allocskb) + " perfctr1 allocskb\n" +
    String(kw->_perfcnt1_pushing) + " perfctr1 pushing\n" +
    String(kw->_perfcnt2_poll) + " perfctr2 poll\n" +
    String(kw->_perfcnt2_refill) + " perfctr2 refill\n" +
    String(kw->_perfcnt2_allocskb) + " perfctr2 allocskb\n" +
    String(kw->_perfcnt2_pushing) + " perfctr2 pushing\n" +
    String(kw->_empty_polls) + " empty polls\n" +
    String(kw->_activations) + " activations\n";
#else
    String();
#endif
}

static String
PollDevice_read_stats(Element *e, void *thunk)
{
  PollDevice *pd = (PollDevice *)e;
  int which = reinterpret_cast<int>(thunk);
  switch (which) {
   case 0:
    return String(pd->_npackets) + "\n";
#if CLICK_DEVICE_THESIS_STATS || CLICK_DEVICE_STATS
   case 1:
    return String(pd->_push_cycles) + "\n";
#endif
#if CLICK_DEVICE_STATS
   case 2:
    return String(pd->_time_poll) + "\n";
   case 3:
    return String(pd->_time_refill) + "\n";
#endif
   default:
    return String();
  }
}

static int
PollDevice_write_stats(const String &, Element *e, void *, ErrorHandler *)
{
  PollDevice *pd = (PollDevice *)e;
  pd->reset_counts();
  return 0;
}

void
PollDevice::add_handlers()
{
  add_read_handler("calls", PollDevice_read_calls, 0);
  add_read_handler("packets", PollDevice_read_stats, 0);
#if CLICK_DEVICE_THESIS_STATS || CLICK_DEVICE_STATS
  add_read_handler("push_cycles", PollDevice_read_stats, (void *)1);
#endif
#if CLICK_DEVICE_STATS
  add_read_handler("poll_cycles", PollDevice_read_stats, (void *)2);
  add_read_handler("refill_dma_cycles", PollDevice_read_stats, (void *)3);
#endif
  add_write_handler("reset_counts", PollDevice_write_stats, 0);
  add_task_handlers(&_task);
}

ELEMENT_REQUIRES(AnyDevice linuxmodule)
EXPORT_ELEMENT(PollDevice)
