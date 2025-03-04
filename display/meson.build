project('ttds', 'c',
  version : '0.1',
  default_options : ['warning_level=3', 'werror=true', 'c_std=c23', 'optimization=g'])

libdrm = dependency('libdrm')
libsystemd = dependency('libsystemd')

add_project_arguments([
  '-pedantic', '-D_POSIX_C_SOURCE=200809L',
  '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=1',
  '-Wno-gnu-zero-variadic-macro-arguments',
], language : 'c')

exe = executable('ttds',
  'main.c', 'testing.c',
  'threads/ui.c', 'threads/commands.c', 'threads/termination.c',
  'rendering/rendering.c', 'rendering/canvas.c',
  'rendering/drm/drm.c', 'rendering/drm/input.c',
  'rendering/mem/mem.c',
  install : true,
  link_args : ['-lm'],
  dependencies : [ libdrm, libsystemd ])

test('basic', exe)
