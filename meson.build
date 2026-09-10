project('tempo', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=0.60.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'default_library=shared',
    'b_ndebug=if-release',
  ])

cc = meson.get_compiler('c')

# Warnings we want, minus the ones that only produce noise for a C API whose
# vtable entries legitimately ignore arguments.
add_project_arguments(cc.get_supported_arguments([
  '-Wno-unused-parameter',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wshadow',
  '-Wpointer-arith',
  '-Wwrite-strings',
  '-Wvla',
]), language: 'c')

tempo_inc = include_directories('include', 'src', 'adapters/mock')

tempo_sources = files(
  'src/base.c',
  'src/time.c',
  'src/util.c',
  'src/trace/trace.c',
  'src/routing/delivery.c',
  'src/federation/spec.c',
  'src/federation/compile.c',
  'src/federation/plan.c',
  'src/federation/graph.c',
  'src/scheduler/scheduler.c',
  'adapters/mock/mock.c',
)

tempo_headers = files(
  'include/tempo/analysis.h',
  'include/tempo/base.h',
  'include/tempo/event.h',
  'include/tempo/federation.h',
  'include/tempo/participant.h',
  'include/tempo/port.h',
  'include/tempo/scheduler.h',
  'include/tempo/tempo.h',
  'include/tempo/time.h',
  'include/tempo/trace.h',
)

# The shipped library exports exactly the TEMPO_API surface, which is also the
# surface the Python bindings dlopen.
libtempo = library('tempo',
  tempo_sources,
  include_directories: tempo_inc,
  c_args: cc.get_supported_arguments(['-fvisibility=hidden']),
  gnu_symbol_visibility: 'hidden',
  version: meson.project_version(),
  soversion: '0',
  install: true)

tempo_dep = declare_dependency(
  link_with: libtempo,
  include_directories: tempo_inc)

# Same code, all symbols visible, so C tests and benchmarks can reach the
# internals when a unit test genuinely needs to.
libtempo_internal = static_library('tempo_internal',
  tempo_sources,
  include_directories: tempo_inc,
  install: false)

tempo_internal_dep = declare_dependency(
  link_with: libtempo_internal,
  include_directories: tempo_inc)

install_headers(tempo_headers, subdir: 'tempo')
install_headers('adapters/mock/tempo_mock.h', subdir: 'tempo')

pkg = import('pkgconfig')
pkg.generate(libtempo,
  name: 'tempo',
  description: 'High-performance co-simulation scheduler',
  subdirs: 'tempo')

subdir('tools/tempo-trace')
subdir('tests')
subdir('benchmarks')

summary({
  'C compiler': cc.get_id() + ' ' + cc.version(),
  'C standard': get_option('c_std'),
  'build type': get_option('buildtype'),
  'library'   : get_option('default_library'),
}, section: 'tempo')
