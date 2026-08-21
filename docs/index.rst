zIPC documentation
==================

zIPC is an experimental chained zero-copy IPC protocol for transferring
ownership of fixed shared-memory buffers between components. The documentation
combines the project's guides with a C API reference extracted from the public
headers by Doxygen and rendered by Sphinx through Breathe.

.. note::

   zIPC v0.x is an experimental prototype. Its public API and shared-memory ABI
   are not stable before v1.0.

Getting started
---------------

.. toctree::
   :maxdepth: 2

   API
   THREE-COMPONENT-EXAMPLE
   NNG-MIGRATION
   TOPOLOGY

Design and integration
----------------------

.. toctree::
   :maxdepth: 2

   ARCHITECTURE
   BACKENDS
   OWNERSHIP
   GUARD_PAGES
   platform-matrix

Reference and project plans
---------------------------

.. toctree::
   :maxdepth: 2

   api-reference
   PACKETRATE-BENCHMARK
   ROADMAP
   DECISIONS
