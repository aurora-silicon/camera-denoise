project('aurora-camera-denoise', 'c',
  version : '0.1.0',
  license : 'MIT',
  meson_version : '>= 0.62',
  default_options : ['c_std=gnu11', 'warning_level=2', 'buildtype=release'])

cc = meson.get_compiler('c')
threads = dependency('threads')
dl = cc.find_library('dl', required : false)
m = cc.find_library('m', required : false)

pkgdatadir = get_option('prefix') / get_option('datadir') / 'aurora-camera-denoise'
backenddir = get_option('prefix') / get_option('libdir') / 'aurora-camera-denoise'
# The user's own networks, copied from their macOS install by install-apple-effects
# (never shipped): <localstatedir>/lib/aurora-camera-denoise/apple
appledir = get_option('prefix') / get_option('localstatedir') / 'lib' / 'aurora-camera-denoise' / 'apple'
sysconfdir = get_option('prefix') / get_option('sysconfdir')
if get_option('prefix') == '/usr'
  sysconfdir = '/etc'
endif

add_project_arguments(
  '-DCDN_DATADIR="@0@"'.format(pkgdatadir),
  '-DCDN_SYSCONFDIR="@0@"'.format(sysconfdir),
  '-DCDN_BACKEND_DIR="@0@"'.format(backenddir),
  '-DCDN_APPLE_DIR="@0@"'.format(appledir),
  '-D_GNU_SOURCE',
  language : 'c')

core = static_library('cdn-core',
  'src/backend.c', 'src/cpu/denoise_cpu.c', 'src/trigger.c', 'src/config.c', 'src/gain.c', 'src/effects.c',
  'src/picture.c', 'src/zoom.c',
  dependencies : [threads, dl, m])
core_dep = declare_dependency(link_with : core, dependencies : [threads, dl, m])

fixture = shared_module('backend-fixture', 'tests/backend_fixture.c', name_prefix : '')
fixture_bad = shared_module('backend-fixture-bad', 'tests/backend_fixture.c',
  name_prefix : '', c_args : ['-DBAD_ABI'])
fixture_legacy = shared_module('backend-fixture-legacy', 'tests/backend_fixture.c',
  name_prefix : '', c_args : ['-DLEGACY'])
test('loader', executable('test_loader', 'tests/test_loader.c', dependencies : core_dep),
  args : [meson.current_build_dir(), fixture.full_path(), fixture_bad.full_path(), fixture_legacy.full_path()])

vulkan = dependency('vulkan', version : '>= 1.1', required : get_option('gpu'))
glslang = find_program('glslangValidator', required : get_option('gpu'))
gpu_headers = false
if vulkan.found()
  gpu_headers = cc.has_header('vulkan/vulkan.h', dependencies : vulkan, required : get_option('gpu'))
endif
if vulkan.found() and glslang.found() and gpu_headers and not get_option('gpu').disabled()
  python = import('python').find_installation('python3')
  spirv = []
  foreach shader : ['filter', 'noise']
    spirv += custom_target(shader + '-spirv',
      input : 'src/gpu' / (shader + '.comp'), output : shader + '.spv',
      depend_files : 'src/gpu/common.glsl',
      command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'src/gpu',
                 '-o', '@OUTPUT@', '@INPUT@'])
  endforeach
  shaders = custom_target('shaders', input : spirv, output : 'shaders.h',
    command : [python, files('tools/embed-spirv.py'), '@OUTPUT@', '@INPUT@'])
  gpu = shared_module('cdn-backend-gpu', 'src/gpu/denoise_gpu.c', shaders,
    name_prefix : '', dependencies : [vulkan, m], install : true, install_dir : backenddir)
  test_gpu = executable('test_gpu', 'tests/test_gpu.c', dependencies : core_dep)
  test('gpu', test_gpu, args : [gpu.full_path()],
    env : ['CDN_GPU_ALLOW_SOFTWARE=1', 'LP_NUM_THREADS=4'], timeout : 180)
  test('gpu-dmabuf', executable('test_gpu_dmabuf', 'tests/test_gpu_dmabuf.c',
    dependencies : [core_dep, vulkan]), args : [gpu.full_path()],
    env : ['CDN_GPU_ALLOW_SOFTWARE=1', 'LP_NUM_THREADS=4'], timeout : 180)
endif

pipewire = dependency('libpipewire-0.3', version : '>= 1.0', required : get_option('daemon'))
if pipewire.found()
  daemon = executable('aurora-camera-denoise', 'src/main.c', 'src/pipewire-frame.c',
    dependencies : [core_dep, pipewire], install : true)
  test('session-config', find_program('tests/check-session-config.sh'), args : [daemon])
  test('buffers', executable('test_buffers', 'tests/test_buffers.c', 'src/pipewire-frame.c',
    dependencies : [core_dep, pipewire]))
  test('formats', executable('test_formats', 'tests/test_formats.c', 'src/pipewire-frame.c',
    dependencies : [core_dep, pipewire]))
endif

# The encoder MCTF temporal backend (V4L2 mem2mem, with the scaler's
# lossless form when it has one), loaded as a plugin.
if host_machine.system() == 'linux' and cc.has_header('linux/videodev2.h')
  shared_module('cdn-backend-ave', 'src/ave/backend_ave.c',
    name_prefix : '', gnu_symbol_visibility : 'hidden',
    install : true, install_dir : backenddir)
endif

# The Neural Engine low-light backend (accel/ane ABI 3, H16G programs from
# data/ane), loaded as a plugin. Needs _Float16.
if host_machine.system() == 'linux' and cc.compiles('_Float16 h = 1.0f; int main(void) { return (int)h - 1; }',
                                                    name : '_Float16')
  ane = shared_module('cdn-backend-ane', 'src/ane/backend_ane.c',
    name_prefix : '', gnu_symbol_visibility : 'hidden',
    install : true, install_dir : backenddir)
  test('ane', executable('test_ane', 'tests/test_ane.c', dependencies : core_dep),
    args : [ane.full_path(), meson.current_build_dir()])
  install_subdir('data/ane', install_dir : pkgdatadir, exclude_files : ['weights.bin'])

  # Portrait, Studio Light and Center Stage with Apple's person segmentation
  # on the Neural Engine (src/apple), loaded instead of the companion plugin
  # when the networks are installed.
  apple_src = files('src/apple/ane_prog.c', 'src/apple/portrait.c', 'src/apple/tuning.c', 'src/apple/warp.c')
  # Center Stage's framing session, checked against traces of Center Stage on macOS: the
  # float operation order matters, so no contraction into fused multiply-adds (gcc fuses by default on arm64).
  cf_framing = static_library('cf-framing', 'src/apple/cf_framing.c', c_args : ['-ffp-contract=off'],
    dependencies : m, pic : true)
  # Per-pixel work on every frame: let the compiler vectorize it whatever the
  # distribution's build type (-O2 left the J815 at ~3x the -O2 clang time).
  # The full-size work on the GPU (Vulkan compute: the quarter-size sums and
  # the composite), the CPU loops when there is none. Same SPIR-V embedding as the denoise backend.
  apple_gpu_src = []
  apple_deps = [m]
  apple_args = ['-O3']
  if vulkan.found() and glslang.found() and gpu_headers and not get_option('gpu').disabled()
    fx_spirv = custom_target('fx-spirv', input : 'src/apple/fx.comp', output : 'fx.spv',
      command : [glslang, '-V', '--target-env', 'vulkan1.1', '-o', '@OUTPUT@', '@INPUT@'])
    quarter_spirv = custom_target('quarter-spirv', input : 'src/apple/quarter.comp', output : 'quarter.spv',
      command : [glslang, '-V', '--target-env', 'vulkan1.1', '-o', '@OUTPUT@', '@INPUT@'])
    fx_shaders = custom_target('fx-shaders', input : [fx_spirv, quarter_spirv], output : 'fx_shaders.h',
      command : [python, files('tools/embed-spirv.py'), '@OUTPUT@', '@INPUT@'])
    apple_gpu_src = [files('src/apple/gpu_fx.c'), fx_shaders]
    apple_deps += vulkan
    apple_args += '-DAPPLE_GPU'
    test('apple-gpu', executable('test_apple_gpu', 'tests/test_apple_gpu.c', apple_src, apple_gpu_src,
      dependencies : [m, vulkan, threads], c_args : ['-DAPPLE_GPU']),
      env : ['CDN_GPU_ALLOW_SOFTWARE=1', 'LP_NUM_THREADS=4'], timeout : 180)
  endif
  apple_fx = shared_module('aurora-camera-effects-apple', 'src/apple/apple_effects.c', apple_src, 'src/zoom.c',
    apple_gpu_src, gnu_symbol_visibility : 'hidden', dependencies : apple_deps + [threads], c_args : apple_args,
    link_with : cf_framing,
    install : true, install_dir : backenddir)
  test('apple', executable('test_apple', 'tests/test_apple.c', apple_src, dependencies : [m, threads],
    link_with : cf_framing))
  test('apple-plugin', executable('test_apple_plugin', 'tests/test_apple_plugin.c', dependencies : dl),
    args : [apple_fx.full_path(), meson.current_build_dir()])
  install_data('tools/install-apple-effects', install_dir : get_option('bindir'),
    install_mode : 'rwxr-xr-x')
endif

executable('aurora-camera-denoise-bench', 'tools/cdn-bench.c',
  dependencies : core_dep, install : true)
executable('aurora-camera-denoise-compare', 'tools/cdn-compare.c',
  dependencies : m, install : true)
executable('aurora-camera-denoise-metrics', 'tools/cdn-metrics.c',
  dependencies : m, install : true)

install_data('data/boards.conf', install_dir : pkgdatadir)
install_data('data/denoise.conf', install_dir : pkgdatadir / 'examples')

unitdir = get_option('systemduserunitdir')
if unitdir == ''
  systemd = dependency('systemd', required : false)
  unitdir = systemd.found() ? systemd.get_variable(pkgconfig : 'systemduserunitdir') : \
    get_option('prefix') / 'lib' / 'systemd' / 'user'
endif
if pipewire.found()
  user_unit = configure_file(input : 'data/aurora-camera-denoise.service.in',
    output : 'aurora-camera-denoise.service',
    configuration : {'bindir' : get_option('prefix') / get_option('bindir')},
    install_dir : unitdir)
  test('user-unit', find_program('sh'), args : [files('tests/check-unit.sh'), user_unit])
endif

test('cpu', executable('test_cpu', 'tests/test_cpu.c', dependencies : core_dep))
test('trigger', executable('test_trigger', 'tests/test_trigger.c', dependencies : core_dep))
test('config', executable('test_config', 'tests/test_config.c', dependencies : core_dep),
  args : [meson.current_build_dir(), files('data/boards.conf')])

test('effects', executable('test_effects', 'tests/test_effects.c', dependencies : core_dep))
test('picture', executable('test_picture', 'tests/test_picture.c', dependencies : core_dep))
test('zoom', executable('test_zoom', 'tests/test_zoom.c', dependencies : core_dep))
executable('aurora-camera-zoom-check', 'tools/cdn-zoom-check.c', dependencies : core_dep, install : true)
