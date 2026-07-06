lib_sources = [
	'drmtest.c',
	'dmabuf_sync_file.c',
	'huc_copy.c',
	'i915/gem.c',
	'i915/gem_context.c',
	'i915/gem_create.c',
	'i915/gem_engine_topology.c',
	'i915/gem_scheduler.c',
	'i915/gem_submission.c',
	'i915/gem_ring.c',
	'i915/gem_mman.c',
	'i915/gem_vm.c',
	'i915/intel_decode.c',
	'i915/intel_drrs.c',
	'i915/intel_fbc.c',
	'i915/intel_memory_region.c',
	'i915/i915_crc.c',
	'i915/i915_dp.c',
	'igt_collection.c',
	'igt_color_encoding.c',
	'igt_configfs.c',
	'igt_facts.c',
	'igt_crc.c',
	'igt_debugfs.c',
	'igt_device.c',
	'igt_device_scan.c',
	'igt_drm_clients.h',
	'igt_drm_fdinfo.c',
        'igt_fs.c',
	'igt_aux.c',
	'igt_dp.c',
	'igt_gt.c',
	'igt_halffloat.c',
	'igt_hwmon.c',
	'igt_matrix.c',
	'igt_os.c',
	'igt_params.c',
	'igt_perf.c',
	'igt_pipe_crc.c',
	'igt_power.c',
	'igt_primes.c',
	'igt_pci.c',
	'igt_rand.c',
	'igt_sriov_device.c',
	'igt_stats.c',
	'igt_syncobj.c',
	'igt_sysfs.c',
	'igt_sysfs_choice.c',
	'igt_sysrq.c',
	'igt_taints.c',
	'igt_thread.c',
	'igt_types.c',
	'igt_vec.c',
	'igt_vgem.c',
	'igt_vkms.c',
	'igt_x86.c',
	'instdone.c',
	'intel_allocator.c',
	'intel_allocator_msgchannel.c',
	'intel_allocator_reloc.c',
	'intel_allocator_simple.c',
	'intel_batchbuffer.c',
	'intel_blt.c',
	'intel_bufops.c',
	'intel_chipset.c',
	'intel_cmds_info.c',
	'intel_common.c',
	'intel_compute.c',
	'intel_compute_square_kernels.c',
	'intel_ctx.c',
	'intel_device_info.c',
	'intel_mmio.c',
	'intel_mocs.c',
	'igt_multigpu.c',
	'intel_pat.c',
	'intel_vram.c',
	'ioctl_wrappers.c',
	'media_spin.c',
	'media_fill.c',
	'gpgpu_fill.c',
	'gpgpu_shader.c',
	'gpu_cmds.c',
	'rendercopy_i915.c',
	'rendercopy_i830.c',
	'rendercopy_gen4.c',
	'rendercopy_gen6.c',
	'rendercopy_gen7.c',
	'rendercopy_gen8.c',
	'rendercopy_gen9.c',
	'runnercomms.c',
	'sw_sync.c',
	'intel_aux_pgtable.c',
	'intel_reg_map.c',
	'intel_iosf.c',
        'intel_wa.c',
	'igt_kms.c',
	'igt_fb.c',
	'igt_core.c',
	'igt_dir.c',
	'igt_draw.c',
	'igt_list.c',
	'igt_map.c',
	'igt_panel.c',
	'igt_pm.c',
	'igt_dummyload.c',
	'igt_store.c',
	'igt_kmod.c',
	'igt_ktap.c',
	'igt_asahi.c',
	'igt_panfrost.c',
	'igt_panthor.c',
	'igt_v3d.c',
	'igt_vc4.c',
	'igt_vmwgfx.c',
	'igt_psr.c',
	'igt_amd.c',
	'igt_edid.c',
	'igt_eld.c',
	'igt_hdr.c',
	'igt_infoframe.c',
	'igt_color.c',
	'igt_color_lut.c',
	'veboxcopy_gen12.c',
	'igt_msm.c',
	'igt_dsc.c',
	'igt_hook.c',
	'xe/xe_gt.c',
	'xe/xe_ioctl.c',
	'xe/xe_legacy.c',
	'xe/xe_mmio.c',
	'xe/xe_query.c',
	'xe/xe_spin.c',
	'xe/xe_sriov_admin.c',
	'xe/xe_sriov_debugfs.c',
	'xe/xe_sriov_provisioning.c',
	'xe/xe_util.c',

	# Vendored libraries:
	'vendor/uwildmat/uwildmat.c',
]

lib_deps = [
	cairo,
	glib,
	libatomic,
	libdrm,
	libdw,
	libkmod,
	libpci,
	libudev,
	math,
	pciaccess,
	pixman,
	pthreads,
	realtime,
	zlib
]

inc = [ inc, include_directories('vendor') ]

if libtsi.found()
	lib_deps += libtsi
	lib_sources += [
		'vendor/unigraf/unigraf.c'
	]
endif

if libdrm_nouveau.found()
	lib_deps += libdrm_nouveau
	lib_sources += [
		'igt_nouveau.c',
		'nouveau/cea0b5.c'
	]
endif

if libdrm_amdgpu.found()
	lib_deps += libdrm_amdgpu
	lib_sources += [
		'amdgpu/amd_memory.c',
		'amdgpu/amd_command_submission.c',
		'amdgpu/amd_compute.c',
		'amdgpu/amd_cs_radv.c',
		'amdgpu/amd_gfx.c',
		'amdgpu/amd_ip_blocks.c',
		'amdgpu/amd_ip_blocks_ex.c',
		'amdgpu/shaders/amd_shaders.c',
		'amdgpu/amd_gfx_v8_0.c',
		'amdgpu/compute_utils/amd_gca.c',
		'amdgpu/compute_utils/amd_dispatch_helpers.c',
		'amdgpu/amd_deadlock_helpers.c',
		'amdgpu/amd_pci_unplug.c',
		'amdgpu/amd_shared_process.c',
		'amdgpu/xalloc.h',
		'amdgpu/amd_cp_dma.c',
		'amdgpu/amd_mem_leak.c',
		'amdgpu/amd_mmd_shared.c',
		'amdgpu/amd_jpeg_shared.c',
		'amdgpu/amd_utils.c',
		'amdgpu/amd_vcn_shared.c'
	]
	if libdrm_amdgpu.version().version_compare('> 2.4.99')
		lib_sources +=[ 'amdgpu/compute_utils/amd_dispatch.c',]
	else
		warning('libdrm <= 2.4.99 found, amdgpu_cs_query_reset_state2 not applicable')
	endif

	# Optional runtime ISA assembly via external llvm-mc tool (no lib dependency needed)
	llvm_mc_prog = find_program(['llvm-mc-20','llvm-mc-19','llvm-mc-18','llvm-mc'], required : false)
	if llvm_mc_prog.found()
		add_project_arguments('-DAMDGPU_LLVM_ENABLED', language : ['c'])
		lib_sources += ['amdgpu/shaders/amd_llvm_asm.c']
		message('Found ' + llvm_mc_prog.path() + ' enabling runtime AMDGPU ISA assembly')
	else
		lib_sources += ['amdgpu/shaders/amd_llvm_asm_stub.c']
		message('llvm-mc not found: using stub implementation (tests will skip)')
	endif

	if libdrm_amdgpu.version().version_compare('>= 2.4.125')
		add_project_arguments('-DAMDGPU_USERQ_ENABLED', language: 'c')
	else
		warning('libdrm < 2.4.125 found, amdgpu user queue support is not enabled')
	endif

endif

if libunwind.found()
	lib_deps += libunwind
else
	inc = [ inc, include_directories('stubs/libunwind') ]
endif

if valgrind.found()
	lib_deps += valgrind
endif

if gsl.found()
	lib_deps += gsl
	lib_sources += [ 'igt_frame.c', 'igt_audio.c' ]
endif

if alsa.found()
	lib_deps += alsa
	lib_sources += 'igt_alsa.c'
endif

if chamelium.found()
	lib_deps += chamelium
	lib_sources += [ 'igt_chamelium.c', 'igt_chamelium_stream.c' ]
endif

if chamelium.found() or libtsi.found()
	lib_sources += 'monitor_edids/monitor_edids_helper.c'
endif

if build_xe_eudebug
	build_info += 'Xe EU debugger test framework enabled.'
	lib_sources += 'xe/xe_eudebug.c'
endif

if get_option('srcdir') != ''
    srcdir = join_paths(get_option('srcdir'), 'tests')
else
    srcdir = join_paths(source_root, 'tests')
endif

if get_option('version_hash') != ''
    vcs_command = ['echo', get_option('version_hash')]
else
    vcs_command = [ 'git', 'log', '-n1', '--pretty=format:g%h' ]
endif

lib_version = vcs_tag(input : 'version.h.in', output : 'version.h',
		      fallback : 'NO-GIT',
		      command : vcs_command )

iga64_assembly_sources = [ 'gpgpu_shader.c', 'gpgpu_fill.c', '../tests/intel/xe_prefetch_fault.c']
libiga64_asms = static_library('iga64_asms',
	iga64_assembly_sources,
	include_directories: inc,
	dependencies : lib_deps,
	c_args : [
	    '-DIGT_DATADIR="@0@"'.format(join_paths(prefix, datadir)),
	    '-DIGT_SRCDIR="@0@"'.format(srcdir),
	    '-DIGT_LOG_DOMAIN="gpu_codes"',
	    '-ffat-lto-objects' ]
)

iga64_codes_targets = []
foreach f: iga64_assembly_sources
	name = f.underscorify() + '.gen.iga64_codes.c'
	iga64_codes_targets += custom_target(name,
		input: [ f + '.gen.iga64_codes.c', libiga64_asms.extract_objects(f) ],
		output: name,
		depend_files: [ 'iga64_macros.h', generate_iga64_codes.path() ],
		command: [ 'env', 'IGT_SRCDIR=@SOURCE_ROOT@', generate_iga64_codes, '-o', '@OUTPUT@', '-i', '@INPUT0@', '@INPUT1@' ],
		console: true,
	)
endforeach

lib_intermediates = []
foreach f: lib_sources
    name = f.underscorify()
    lib = static_library('igt-' + name,
	[ f, lib_version ],
	include_directories: inc,
	dependencies : lib_deps,
	c_args : [
	    '-DIGT_DATADIR="@0@"'.format(join_paths(prefix, datadir)),
	    '-DIGT_SRCDIR="@0@"'.format(srcdir),
	    '-DIGT_LOG_DOMAIN="@0@"'.format(f.split('.')[0]),
            '-DIGT_IMGDIR="@0@"'.format(imgdir),
	])

    lib_intermediates += lib
endforeach

lib_intermediates += static_library('iga64_gpu_codes', iga64_codes_targets)

run_target('rebuild_iga64',
    command : [ 'bash', '-c', 'ninja -t clean lib/libiga64_asms.a && FORCE_IGA64_REBUILD=1 ninja lib/libiga64_gpu_codes.a' ])

lib_igt_build = shared_library('igt',
    ['dummy.c'],
    link_whole: lib_intermediates,
    dependencies: lib_deps,
    install : true,
    soversion : '0',
)

lib_igt = declare_dependency(link_with : lib_igt_build,
			    include_directories : inc)

igt_deps = [ lib_igt ] + lib_deps

lin_igt_chipset_build = static_library('igt_chipset',
                                       ['intel_chipset.c',
					'intel_device_info.c',
					'intel_cmds_info.c'],
                                       include_directories : inc)

lib_igt_chipset = declare_dependency(link_with : lin_igt_chipset_build,
                                     include_directories : inc)

lib_igt_perf_build = static_library('igt_perf',
	['igt_perf.c'],
	include_directories : inc)

lib_igt_perf = declare_dependency(link_with : lib_igt_perf_build,
				  include_directories : inc)

scan_dep = [
	glib,
	libpci,
	libudev,
]

lib_igt_device_scan_build = static_library('igt_device_scan',
	['igt_device_scan.c',
	'igt_list.c',
	'igt_map.c',
	'igt_tools_stub.c',
	'intel_device_info.c',
	'intel_cmds_info.c',
	],
	dependencies : scan_dep,
	include_directories : inc)

lib_igt_device_scan = declare_dependency(link_with : lib_igt_device_scan_build,
				  include_directories : inc)

lib_igt_drm_clients_build = static_library('igt_drm_clients',
        ['igt_drm_clients.c'],
        include_directories : inc)

lib_igt_drm_clients = declare_dependency(link_with : lib_igt_drm_clients_build,
				         include_directories : inc)

lib_igt_drm_fdinfo_build = static_library('igt_drm_fdinfo',
	['igt_drm_fdinfo.c'],
	include_directories : inc)

lib_igt_drm_fdinfo = declare_dependency(link_with : lib_igt_drm_fdinfo_build,
				  include_directories : inc)

lib_igt_profiling_build = static_library('igt_profiling',
	['igt_profiling.c'],
	include_directories : inc)

lib_igt_profiling = declare_dependency(link_with : lib_igt_profiling_build,
				        include_directories : inc)

i915_perf_files = [
  'igt_list.c',
  'i915/perf.c',
  'i915/perf_data_reader.c',
]

i915_perf_hardware = [
  'hsw',
  'bdw', 'chv',
  'sklgt2', 'sklgt3', 'sklgt4',
  'kblgt2', 'kblgt3',
  'cflgt2', 'cflgt3',
  'bxt', 'glk',
  'cnl',
  'icl', 'ehl',
  'tglgt1', 'tglgt2', 'rkl', 'dg1', 'adl',
  'acmgt1', 'acmgt2', 'acmgt3',
  'mtlgt2', 'mtlgt3',
]

i915_xml_files = []
foreach hw : i915_perf_hardware
  i915_xml_files += files('i915/perf-configs/oa-@0@.xml'.format(hw))
endforeach

i915_perf_files += custom_target(
  'i915-perf-equations',
  input : [ 'i915/perf-configs/perf-equations-codegen.py' ] + i915_xml_files,
  output : [ 'i915_perf_equations.c', 'i915_perf_equations.h' ],
  command : [
    python3, '@INPUT0@',
    '--code', '@OUTPUT0@',
    '--header', '@OUTPUT1@',
    i915_xml_files,
  ])

foreach hw : i915_perf_hardware
  i915_perf_files += custom_target(
    'i915-perf-registers-@0@'.format(hw),
    input : [ 'i915/perf-configs/perf-registers-codegen.py',
              'i915/perf-configs/oa-@0@.xml'.format(hw) ],
    output : [ 'i915_perf_registers_@0@.c'.format(hw),
               'i915_perf_registers_@0@.h'.format(hw), ],
    command : [
      python3, '@INPUT0@',
      '--code', '@OUTPUT0@',
      '--header', '@OUTPUT1@',
      '--xml-file', '@INPUT1@'
    ])
  i915_perf_files += custom_target(
    'i915-perf-metrics-@0@'.format(hw),
    input : [ 'i915/perf-configs/perf-metricset-codegen.py',
              'i915/perf-configs/oa-@0@.xml'.format(hw) ],
    output : [ 'i915_perf_metrics_@0@.c'.format(hw),
               'i915_perf_metrics_@0@.h'.format(hw), ],
    command : [
      python3, '@INPUT0@',
      '--code', '@OUTPUT0@',
      '--header', '@OUTPUT1@',
      '--equations-include', 'i915_perf_equations.h',
      '--registers-include', 'i915_perf_registers_@0@.h'.format(hw),
      '--xml-file', '@INPUT1@',
    ])
endforeach

lib_igt_i915_perf_build = shared_library(
  'i915_perf',
  i915_perf_files,
  dependencies: lib_igt_chipset,
  include_directories : inc,
  install: true,
  soversion: '1.5')

lib_igt_i915_perf = declare_dependency(
  link_with : lib_igt_i915_perf_build,
  include_directories : inc)

install_headers(
  'igt_list.h',
  'intel_chipset.h',
  'i915/perf.h',
  'i915/perf_data.h',
  'i915/perf_data_reader.h',
  subdir : 'i915-perf'
)

xe_oa_files = [
  'igt_list.c',
  'xe/xe_oa.c',
  'xe/xe_oa_data_reader.c',
]

xe_oa_hardware = [
  'tglgt1', 'tglgt2',
  'dg1', 'rkl', 'adl',
  'acmgt1', 'acmgt2', 'acmgt3',
  'pvc',
  'mtlgt2', 'mtlgt3',
  'lnl',
  'bmg',
  'ptl',
  'cri',
]

xe_xml_files = []
foreach hw : xe_oa_hardware
  xe_xml_files += files('xe/oa-configs/oa-@0@.xml'.format(hw))
endforeach

xe_oa_files += custom_target(
  'xe-oa-equations',
  input : [ 'xe/oa-configs/oa-equations-codegen.py' ] + xe_xml_files,
  output : [ 'xe_oa_equations.c', 'xe_oa_equations.h' ],
  command : [
    python3, '@INPUT0@',
    '--code', '@OUTPUT0@',
    '--header', '@OUTPUT1@',
    xe_xml_files,
  ])

foreach hw : xe_oa_hardware
  xe_oa_files += custom_target(
    'xe-oa-registers-@0@'.format(hw),
    input : [ 'xe/oa-configs/oa-registers-codegen.py',
              'xe/oa-configs/oa-@0@.xml'.format(hw) ],
    output : [ 'xe_oa_registers_@0@.c'.format(hw),
               'xe_oa_registers_@0@.h'.format(hw), ],
    command : [
      python3, '@INPUT0@',
      '--code', '@OUTPUT0@',
      '--header', '@OUTPUT1@',
      '--xml-file', '@INPUT1@'
    ])
  xe_oa_files += custom_target(
    'xe-oa-metrics-@0@'.format(hw),
    input : [ 'xe/oa-configs/oa-metricset-codegen.py',
              'xe/oa-configs/oa-@0@.xml'.format(hw) ],
    output : [ 'xe_oa_metrics_@0@.c'.format(hw),
               'xe_oa_metrics_@0@.h'.format(hw), ],
    command : [
      python3, '@INPUT0@',
      '--code', '@OUTPUT0@',
      '--header', '@OUTPUT1@',
      '--equations-include', 'xe_oa_equations.h',
      '--registers-include', 'xe_oa_registers_@0@.h'.format(hw),
      '--xml-file', '@INPUT1@',
    ])
endforeach

lib_igt_xe_oa_build = shared_library(
  'xe_oa',
  xe_oa_files,
  dependencies: [lib_igt_chipset,lib_igt,pciaccess],
  include_directories : inc,
  install: true,
  soversion: '1.5')

lib_igt_xe_oa = declare_dependency(
  link_with : lib_igt_xe_oa_build,
  include_directories : inc)

install_headers(
  'igt_list.h',
  'intel_chipset.h',
  'xe/xe_oa.h',
  'xe/xe_oa_data.h',
  'xe/xe_oa_data_reader.h',
  subdir : 'xe-oa'
)

pkgconf = configuration_data()

pkgconf.set('prefix', get_option('prefix'))
pkgconf.set('exec_prefix', '${prefix}')
pkgconf.set('libdir', '${prefix}/@0@'.format(get_option('libdir')))
pkgconf.set('includedir', '${prefix}/@0@'.format(get_option('includedir')))
pkgconf.set('i915_perf_version', '1.5.1')
pkgconf.set('xe_oa_version', '1.0.0')

configure_file(
  input : 'i915-perf.pc.in',
  output : 'i915-perf.pc',
  configuration : pkgconf,
  install_dir : pkgconfigdir)

configure_file(
  input : 'xe-oa.pc.in',
  output : 'xe-oa.pc',
  configuration : pkgconf,
  install_dir : pkgconfigdir)

subdir('tests')
