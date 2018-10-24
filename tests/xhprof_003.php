<?php

include_once dirname(__FILE__).'/common.php';

class C {
  private static $_static_attr = "i am a class static";
  private $_attr;
  function __construct($attr) {
    echo "In constructor...\n";
    $this->_attr = $attr;
  }

  private static function inner_static() {
    return C::$_static_attr;
  }

  public static function outer_static() {
    return C::inner_static();
  }

  public function get_attr() {
    return $this->_attr;
  }

  function __destruct() {
    echo "Destroying class {$this->_attr}\n";
  }
}


xhprof_enable();

// static methods
echo C::outer_static() . "\n";

// constructor
$obj = new C("Hello World");

// instance methods
$obj->get_attr();

// destructor
$obj = null;


$output = xhprof_disable();

echo "Profiler data for 'Class' tests:\n";
print_canonical($output);
echo "\n";

?>
