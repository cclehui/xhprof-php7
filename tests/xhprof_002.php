<?php

include_once dirname(__FILE__).'/common.php';

// dummy wrapper to test indirect recursion
function bar($depth, $use_direct_recursion) {
  foo($depth, $use_direct_recursion);
}

function foo($depth, $use_direct_recursion = false) {
  if ($depth > 0) {
    if ($use_direct_recursion)
      foo($depth - 1, $use_direct_recursion);
    else
      bar($depth - 1, $use_direct_recursion);
  }
}


xhprof_enable();
foo(4, true);
$output = xhprof_disable();

echo "Direct Recursion\n";
print_canonical($output);
echo "\n";


xhprof_enable();
foo(4, false);
$output = xhprof_disable();

echo "Indirect Recursion\n";
print_canonical($output);
echo "\n";

?>
