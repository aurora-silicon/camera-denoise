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
sysconfdir = get_option('prefix') / get_option('sysconfdir')
if get_option('prefix') == '/usr'
  sysconfdir = '/etc'
endif

add_project_arguments(
  '-DCDN_DATADIR="@0@"'.format(pkgdatadir),
  '-DCDN_SYSCONFDIR="@0@"'.format(sysconfdir),
  '-DCDN_BACKEND_DIR="@0@"'.format(backenddir),
  '-D_GNU_SOURCE',
  language : 'c')

core = static_library('cdn-core',
  'src/backend.c', 'src/cpu/denoise_cpu.c', 'src/trigger.c', 'src/config.c', 'src/gain.c',
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
if vulkan.found() and glslang.found() and not get_option('gpu').disabled()
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
endif

pipewire = dependency('libpipewire-0.3', version : '>= 1.0', required : get_option('daemon'))
if pipewire.found()
  executable('aurora-camera-denoise', 'src/main.c', 'src/pipewire-frame.c',
    dependencies : [core_dep, pipewire], install : true)
  test('buffers', executable('test_buffers', 'tests/test_buffers.c', 'src/pipewire-frame.c',
    dependencies : [core_dep, pipewire]))
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
  configure_file(input : 'data/aurora-camera-denoise.service.in',
    output : 'aurora-camera-denoise.service',
    configuration : {'bindir' : get_option('prefix') / get_option('bindir')},
    install_dir : unitdir)
endif

test('cpu', executable('test_cpu', 'tests/test_cpu.c', dependencies : core_dep))
test('trigger', executable('test_trigger', 'tests/test_trigger.c', dependencies : core_dep))
test('config', executable('test_config', 'tests/test_config.c', dependencies : core_dep),
  args : [meson.current_build_dir()])
