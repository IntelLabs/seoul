/**
 * VCPU to VBios bridge.
 *
 * Copyright (C) 2010, Bernhard Kauer <bk@vmmon.org>
 *
 * This file is part of Vancouver.
 *
 * Vancouver is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Vancouver is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 */

#include "nul/motherboard.h"
#include "nul/vcpu.h"
#include "executor/bios.h"

class VBios : public StaticReceiver<VBios>
{
  VCpu *_vcpu;
  DBus<MessageBios> &_bus_bios;
  unsigned char _resetvector[16];
  enum {
    BIOS_BASE = 0xf0000,
  };

public:
  bool receive(CpuMessage &msg) {
    if (msg.type != CpuMessage::TYPE_SINGLE_STEP) return false;

    CpuState *cpu = msg.cpu;
    if (cpu->pm() && !cpu->v86() || !in_range(cpu->cs.base + cpu->eip, BIOS_BASE, BiosCommon::MAX_VECTOR)) return false;

    COUNTER_INC("VB");

    unsigned irq =  (cpu->cs.base + cpu->eip) - BIOS_BASE;

    if (irq == BiosCommon::RESET_VECTOR) {
      // initialize realmode idt
      unsigned value = (BIOS_BASE >> 4) << 16;
      for (unsigned i=0; i<256; i++) {
	MessageMem msg(false, i*4, &value);
	_vcpu->mem.send(msg);
	value++;
      }
    }


    /**
     * We jump to the last instruction in the 16-byte reset area where
     * we provide an IRET instruction to the instruction emulator.
     */
    cpu->cs.sel  = BIOS_BASE >> 4;
    cpu->cs.base = BIOS_BASE;
    cpu->eip = 0xffff;

    MessageBios msg1(_vcpu, cpu, irq);
    if (irq != BiosCommon::RESET_VECTOR ? _bus_bios.send(msg1, true) : _bus_bios.send_fifo(msg1)) {

      // we have to propagate the flags to the user stack!
      unsigned flags;
      MessageMem msg2(true, cpu->ss.base + cpu->esp + 4, &flags);
      _vcpu->mem.send(msg2);
      flags = flags & ~0xffffu | cpu->efl & 0xffffu;
      msg2.read = false;
      _vcpu->mem.send(msg2);
      msg.mtr_out |= msg1.mtr_out;
      return true;
    }
    return false;
  }

  /**
   * The memory read routine for the last 16byte below 4G and below 1M.
   */
  bool receive(MessageMem &msg)
  {
    if (!msg.read || !in_range(msg.phys, 0xfffffff0, 0x10) && !in_range(msg.phys, BIOS_BASE + 0xfff0, 0x10))  return false;
    *msg.ptr = *reinterpret_cast<unsigned *>(_resetvector + (msg.phys & 0xc));
    return true;
  }


  VBios(VCpu *vcpu, DBus<MessageBios> &bus_bios) : _vcpu(vcpu), _bus_bios(bus_bios) {

    // initialize the reset vector with noops
    memset(_resetvector, 0x90, sizeof(_resetvector));
    // realmode longjump to reset vector
    _resetvector[0x0] = 0xea;
    _resetvector[0x1] = BiosCommon::RESET_VECTOR & 0xff;
    _resetvector[0x2] = BiosCommon::RESET_VECTOR >> 8;
    _resetvector[0x3] = (BIOS_BASE >> 4) & 0xff;
    _resetvector[0x4] = BIOS_BASE >> 12;

    // the iret for do_iret()
    _resetvector[0xf] = 0xcf;
  }

};

PARAM(vbios,
      if (!mb.last_vcpu) Logging::panic("no VCPU for this VBIOS");

      VBios *dev = new VBios(mb.last_vcpu, mb.bus_bios);
      mb.last_vcpu->executor.add(dev, &VBios::receive_static<CpuMessage>);
      mb.last_vcpu->mem.add(dev,      &VBios::receive_static<MessageMem>);
      ,
      "vbios - create a bridge between VCPU and the BIOS bus.");
