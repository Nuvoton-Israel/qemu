NPCM845 KCS (brief)
===================

The NPCM845 SoC provides three BMC-side KCS channels at ``0xf0007000``
(``hw/misc/npcm_kcs.c``). They share one BMC interrupt and use the APB1 clock.
Registers are byte-wide; wider accesses are rejected.

Channel ``n`` (1-3) starts at ``0xf0007000 + 0x0c + (n - 1) * 0x12``.

.. list-table::
   :header-rows: 1

   * - Offset
     - Register
     - Behavior
   * - ``0x00``
     - ST
     - Status: OBF bit 0, IBF bit 1, CMD bit 3
   * - ``0x02``
     - DO
     - BMC output byte
   * - ``0x04``
     - DI
     - BMC input byte; reading clears IBF
   * - ``0x06``
     - DOC
     - BMC output byte, requests SCI
   * - ``0x08``
     - DOM
     - BMC output byte, requests SMI
   * - ``0x0a``
     - DIC
     - BMC input byte, requests SCI
   * - ``0x0c``
     - CTL
     - BMC IRQ enables, SCI pulse mode and polarity
   * - ``0x0e``
     - IC
     - Host IRQ control, SCI/SMI software requests, SMI pulse mode and polarity
   * - ``0x10``
     - IE
     - Host IRQ, SCI and SMI enables, hardware/software source selection

Host access
-----------

The host side is simulated through QOM properties at ``/machine/soc/kcs``.
For channel ``n``: writing ``host-commandn`` or ``host-datan`` sends a command
or data byte; ``host-statusn`` reads status; reading ``host-datan`` returns the
BMC output byte, and writing ``host-ackn`` clears OBF. ``host-irqn``,
``host-scin`` and ``host-smin`` report the output levels (SCI/SMI are
active-low by default).

Example::

  {"execute": "qom-set", "arguments": {
    "path": "/machine/soc/kcs", "property": "host-command1", "value": 97}}
  {"execute": "qom-get", "arguments": {
    "path": "/machine/soc/kcs", "property": "host-status1"}}

SCI and SMI
-----------

SCI is requested by DOC writes and DIC reads, SMI by DOM writes. Outputs are
level or 1-16 APB1-cycle pulses, with configurable polarity, and are also
exposed as GPIOs ``sci[0..2]`` and ``smi[0..2]``. They are not routed to a host
CPU or eSPI model.

Optionally, ``shm-path``, ``doorbell-in`` and ``doorbell-out`` expose the host
side through shared memory and Unix datagram doorbells for an external peer.

Trace events: ``-trace 'npcm_kcs_*'``.
