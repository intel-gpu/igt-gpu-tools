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
  'xe.bmg.dkms.kv6.6.blocklist.txt',
  'xe.bmg.eudebug.blocklist.txt',
  'xe.cri.core.blocklist.txt',
  'xe.cri.premerge.testlist',
  'xe.dkms_kv5.15.blocklist.txt',
  'xe.eudebug.blocklist.txt',
  'xe.fast-feedback.testlist',
  'xe-fast-feedback.testlist',
  'xe.fast-feedback-chamelium-only.testlist',
  'xe-fast-feedback-chamelium-only.testlist',
  'xe.lnl.core.blocklist.txt',
  'xe.lnl.display.blocklist.txt',
  'xe.lnl.eudebug.blocklist.txt',
  'xe.multigpu.blocklist.txt',
  'xe.nvl.core.blocklist.txt',
  'xe.nvl.display.blocklist.txt',
  'xe.pm.d3cold.testlist',
  'xe.ptl.core.blocklist.txt',
  'xe.ptl.display.blocklist.txt',
  'xe.sriov-vf.blocklist.txt',
  'xe-sriov-vf.blocklist.txt',
  'xe.sriov-vm.blocklist.txt',
  'xe-sriov-vm.blocklist.txt',
  'xe.cri.sriov-vm.blocklist.txt',
  'xe.wcl.core.blocklist.txt',
]

hooks_files = [
  'hooks/common.helper.sh',
  'hooks/copy_guc_log_on_fail.sh',
  'hooks/copy_guc_log_on_fail.allowlist',
  'hooks/hook-date.sh',
  'hooks/hook-date.allowlist',
  'hooks/hooks-wrapper.sh',
]

if meson.version().version_compare('<1.3.0')
  install_data(sources : intelci_files, install_dir : datadir)
  install_data(sources : hooks_files, install_dir : datadir + '/hooks')
else
  install_data(sources : intelci_files, install_dir : datadir, follow_symlinks : false)
  install_data(sources : hooks_files, install_dir : datadir + '/hooks', follow_symlinks : false)
endif
