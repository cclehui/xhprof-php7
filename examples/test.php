<?php

echo "xxxxxxxxx\n";

function bar($x) {
  if ($x > 0) {
    bar($x - 1);
  }
}

function foo() {
  for ($idx = 0; $idx < 5; $idx++) {
    bar($idx);
    $x = strlen("abc");
  }
}

// start profiling
//$options = ['xhprof_test' => 11111, 'bar'];
$options = [
    //'ignored_functions' => ['bar'],
    'track_functions' => ['test', 'bar'],
];
//$options = ['ignored_functions' => 'bar'];
xhprof_enable(0, $options);
//xhprof_enable(100, "xxxxxxxxxxxxx121111111111x");
//die;


echo "ssssssssssss\n";

xhprof_test();

echo "ttttttttttt\n";
// run program
foo();

test();

// stop profiler
$xhprof_data = xhprof_disable();

echo "rrrrrrrrrrrr\n";

xhprof_test();

// display raw xhprof data for the profiler run
var_dump($xhprof_data);
echo "zzzzzzzzzzzzz\n";


function  test() {

    echo "---------------------\n";
}

