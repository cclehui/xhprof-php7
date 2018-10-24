PHP_ARG_ENABLE(xhprof, whether to enable xhprof support,
[ --enable-xhprof      Enable xhprof support])

if test "$PHP_XHPROF" != "no"; then
  PHP_NEW_EXTENSION(xhprof, xhprof.c, $ext_shared)
fi

if test -z "$PHP_DEBUG" ; then
  AC_ARG_ENABLE(debug, [--enable-debug compile with debugging system], [PHP_DEBUG=$enableval],[PHP_DEBUG=no] )
fi
