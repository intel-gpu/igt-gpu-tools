# Keep alphabetically sorted by default

intelci_files = [
  'README',
  'chamelium-only.testlist',
  'fast-feedback.testlist',
  'fast-feedback-chamelium-only.testlist',
  'i915.blocklist.txt',
  'i915.blocklist-pre-merge.txt',
  'i915.fast-feedback.testlist',
  'i915.fast-feedback-chamelium-only.testlist',
  'meta.testlist',
  'xe.blocklist.txt',
  'xe.bmg.core.blocklist.txt',
  'xe.bmg.display.blocklist.txt',
  'xe.bmg.dkms.blocklist.txt',
  'xe.bmg.eudebug.blocklist.txt',
  'xe.eudebug.blocklist.txt',
  'xe.fast-feedback.testlist',
  'xe-fast-feedback.testlist',
  'xe.fast-feedback-chamelium-only.testlist',
  'xe-fast-feedback-chamelium-only.testlist',
  'xe.lnl.core.blocklist.txt',
  'xe.lnl.display.blocklist.txt',
  'xe.lnl.eudebug.blocklist.txt',
  'xe.multigpu.blocklist.txt',
  'xe.ptl.core.blocklist.txt',
  'xe.ptl.display.blocklist.txt',
  'xe.sriov-vf.blocklist.txt',
  'xe-sriov-vf.blocklist.txt',
  'xe.sriov-vm.blocklist.txt',
  'xe-sriov-vm.blocklist.txt',
  'xe.kv6.17-only.blocklist.txt',
]

if meson.version().version_compare('<1.3.0')
  install_data(sources : intelci_files, install_dir : datadir)
else
  install_data(sources : intelci_files, install_dir : datadir, follow_symlinks : false)
endif
