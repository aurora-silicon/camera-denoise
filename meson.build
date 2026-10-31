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
  'src/backend.c', 'src/cpu/denoise_cpu.c', 'src/trigger.c', 'src/config.c', 'src/gain.c', 'src/effects.c',
  dependencies : [threads, dl, m])
core_dep = declare_dependency(link_with : core, dependencies : [threads, dl, m])

pipewire = dependency('libpipewire-0.3', version : '>= 1.0', required : get_option('daemon'))
if pipewire.found()
  daemon = executable('aurora-camera-denoise', 'src/main.c',
    dependencies : [core_dep, pipewire], install : true)
  test('session-config', find_program('tests/check-session-config.sh'), args : [daemon])
endif

executable('aurora-camera-denoise-bench', 'tools/cdn-bench.c',
  dependencies : core_dep, install : true)
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

test('effects', executable('test_effects', 'tests/test_effects.c', dependencies : core_dep))
