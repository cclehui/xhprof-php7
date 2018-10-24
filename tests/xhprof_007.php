<?php

include_once dirname(__FILE__).'/common.php';

$xhprof_ignored_functions = array( 'ignored_functions' =>
                                      array('call_user_func',
                                            'call_user_func_array',
                                            'my_call_user_func_safe',
                                            'my_call_user_func_array_safe'));
function bar() {
  return 1;
}

function foo($x) {
  $sum = 0;
  for ($idx = 0; $idx < 2; $idx++) {
     $sum += bar();
  }
  echo @"hello: {$x}\n" ;
  return @strlen("hello: {$x}");
}

function foo_array($x1, $x2 = 'test') {
  $sum = 0;
  $x = array($x1, $x2);
  foreach ($x as $idx) {
     $sum += bar();
  }
  echo @"hello: {$x[0]}{$x[1]}\n";
  return @strlen("hello: {$x[0]} {$x[1]}");
}

function my_call_user_func_safe($function, $args = 'my_safe') {
  if (!is_callable($function, true)) {
    throw new Exception('my_call_user_func_safe() invoked without ' .
                        'a valid callable.');
  }

  call_user_func($function, array($args));
}

function my_call_user_func_array_safe($function, $args = array()) {
  if (!is_callable($function, true)) {
    throw new Exception('my_call_user_func_array_safe() invoked without ' .
                        'a valid callable.');
  }

  call_user_func_array($function, $args);
}


class test_call_user_func {
  function test_call_user_func($test_func = 'foo',
                               $arg1      = 'user_func test') {
    call_user_func($test_func, $arg1);
  }
}

function test_call_user_func_array($test_func = 'foo_array',
                                   $arg1      = array(0 => 'user_func_array',
                                                      'test')) {
  call_user_func_array($test_func, $arg1);
}

function test_my_call_user_func_safe($test_func = 'foo',
                                     $arg1      = 'my_user_func_safe test') {
  my_call_user_func_safe($test_func, $arg1);
}

function test_my_call_user_func_array_safe(
                                   $test_func = 'foo_array',
                                   $arg1      = array('my_user_func_array_safe',
                                                      'test')) {
  my_call_user_func_array_safe($test_func, $arg1);
}


// 1: Sanity test a simple profile run
echo "Part 1: Default Flags\n";
xhprof_enable(0, $xhprof_ignored_functions);
foo("this is a test");
$array_arg = array();
$array_arg[] = 'calling ';
$array_arg[] = 'foo_array';
foo_array($array_arg);

$output = xhprof_disable();
echo "Part 1 output:\n";
print_canonical($output);
echo "\n";

// 2a: Sanity test ignoring call_user_func
echo "Part 2a: Ignore call_user_func\n";
xhprof_enable(0, $xhprof_ignored_functions);
$indirect_foo = new test_call_user_func('foo');
$output = xhprof_disable();
echo "Part 2a output:\n";
print_canonical($output);
echo "\n";

// 2b: Confirm that profiling without parameters still works
echo "Part 2b: Standard profile without parameters\n";
xhprof_enable();
$indirect_foo = new test_call_user_func('foo');
$output = xhprof_disable();
echo "Part 2b output:\n";
print_canonical($output);
echo "\n";

// 2c: Confirm that empty array of ignored functions works
echo "Part 2c: Standard profile with empty array of ignored functions\n";
xhprof_enable(0, array());
$indirect_foo = new test_call_user_func('foo');
$output = xhprof_disable();
echo "Part 2c output:\n";
print_canonical($output);
echo "\n";

// 3: Sanity test ignoring call_user_func_array
echo "Part 3: Ignore call_user_func_array\n";
xhprof_enable(XHPROF_FLAGS_CPU, $xhprof_ignored_functions);
test_call_user_func_array('foo_array', $array_arg);
$output = xhprof_disable();
echo "Part 3 output:\n";
print_canonical($output);
echo "\n";

// 4: Sanity test ignoring my_call_user_func_safe
echo "Part 4: Ignore my_call_user_func_safe\n";
xhprof_enable(0, $xhprof_ignored_functions);
test_my_call_user_func_safe('foo');
$output = xhprof_disable();
echo "Part 4 output:\n";
print_canonical($output);
echo "\n";

// 5a: Sanity test ignoring my_call_user_func_array_safe and strlen
echo "Part 5a: Ignore my_call_user_func_array_safe and strlen\n";
$tmp1 = $xhprof_ignored_functions['ignored_functions'];
$tmp1[] = 'strlen';
$ignore_strlen_also = array('ignored_functions' => $tmp1);
xhprof_enable(XHPROF_FLAGS_MEMORY, $ignore_strlen_also);
test_my_call_user_func_array_safe('foo_array');
$output = xhprof_disable();
echo "Part 5a output:\n";
print_canonical($output);
echo "\n";

// 5b: Sanity test to not ignore call_user_func variants
echo "Part 5b: Profile call_user_func_array and my_call_user_func_array_safe\n";
xhprof_enable(XHPROF_FLAGS_MEMORY, array());
test_my_call_user_func_array_safe('foo_array');
$output = xhprof_disable();
echo "Part 5b output:\n";
print_canonical($output);
echo "\n";

// 5c: Sanity test to only ignore my_call_user_func_array_safe
echo "Part 5c: Only ignore call_user_func_array\n";
$xhprof_ignored_functions = array('ignored_functions' =>
                                  'my_call_user_func_array_safe');
xhprof_enable(XHPROF_FLAGS_MEMORY, $xhprof_ignored_functions);
test_my_call_user_func_array_safe('foo_array');
$output = xhprof_disable();
echo "Part 5c output:\n";
print_canonical($output);
echo "\n";

?>
