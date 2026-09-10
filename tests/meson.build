tempo_tests = [
  ['time',       'test_time.c'],
  ['trace',      'test_trace.c'],
  ['compile',    'test_compile.c'],
  ['graph',      'test_graph.c'],
  ['scheduler',  'test_scheduler.c'],
]

foreach t : tempo_tests
  exe = executable('test_' + t[0],
    files(t[1]),
    dependencies: tempo_internal_dep,
    install: false)
  test(t[0], exe, timeout: 60)
endforeach

# The Python control plane is tested with pytest, against the library that was
# just built.
#
# Use the interpreter Meson itself is running under rather than whatever
# `pytest` is first on PATH: in a virtual env those are usually different, and
# the wrong one fails with a missing cffi rather than a real result. If that
# interpreter cannot import what the tests need, say so and skip -- a C-only
# checkout should still build and test cleanly.
py = import('python').find_installation(required: false)
if py.found()
  probe = run_command(py, '-c', 'import cffi, pytest', check: false)
  if probe.returncode() == 0
    test('python', py,
      args: ['-m', 'pytest', '-q', meson.current_source_dir()],
      env: {
        'TEMPO_LIB': libtempo.full_path(),
        'PYTHONPATH': meson.project_source_root() / 'python',
      },
      depends: [libtempo],
      timeout: 120)
  else
    message('skipping Python tests: @0@ cannot import cffi and pytest'.format(
      py.full_path()))
  endif
else
  message('skipping Python tests: no Python installation found')
endif
