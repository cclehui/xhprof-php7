<?php

function loader() {
  // <empty>
}

spl_autoload_register('loader', $throw = true);

xhprof_enable();

class_exists('ThisClassDoesNotExist');
echo "OK\n";

