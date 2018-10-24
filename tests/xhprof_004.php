<?php

include_once dirname(__FILE__).'/common.php';

xhprof_enable();

// Include File:
//
// Note: the 2nd and 3rd attempts should be no-ops and
// will not show up in the profiler data. Only the first
// one should.

include_once dirname(__FILE__).'/xhprof_004_inc.php';
include_once dirname(__FILE__).'/xhprof_004_inc.php';
include_once dirname(__FILE__).'/xhprof_004_inc.php';


// require_once:
// Note: the 2nd and 3rd attempts should be no-ops and
// will not show up in the profiler data. Only the first
// one should.

require_once dirname(__FILE__).'/xhprof_004_require.php';
require_once dirname(__FILE__).'/xhprof_004_require.php';
require_once dirname(__FILE__).'/xhprof_004_require.php';

$output = xhprof_disable();

echo "Test for 'include_once' & 'require_once' operation\n";
print_canonical($output);
echo "\n";

?>
