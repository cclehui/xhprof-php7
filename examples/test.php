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
$options = [
    //'ignored_functions' => ['bar'],
    //'track_functions' => ['test', 'bar', 'foo'],
    //'track_functions' => ['bar', 'foo', 'TempUtil:test'],
    'track_functions' => ['bar', 'foo', 'test'],
    //'track_functions' => ['test'],
];
//$options = ['ignored_functions' => 'bar'];
xhprof_enable(0, $options);

//xhprof_test();

$start_ts = microtime(true);

//for ($i = 1; $i <= 20; $i++) {
//for ($i = 1; $i <= 10000000; $i++) {
for ($i = 1; $i <= 20000000; $i++) {
    test();
    //TempUtil::test();
    //bar(0);
    if ($i % 100 == 0) {
        //foo();
        //bar(0);
        //$memory = memory_get_usage(true);
        //$memory_mb = intval($memory / 1000000);
        //echo "$i\tmemory_usage\t$memory\t$memory_mb\n" ;
    }
}


$xhprof_data = xhprof_disable();

//var_dump($xhprof_data);
print_r($xhprof_data);

$time_cost = microtime(true) - $start_ts;

echo "total_ts\t$time_cost\n"; 

function test() {

    //echo "---------------------\n";
}

class TempUtil{

    public static function test() {

        //echo "---------------------\n";
    }
}

