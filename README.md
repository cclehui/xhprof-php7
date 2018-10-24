# xhprof-php7
facebook开源的xhprof 扩展不支持Php7， 只能在php5及以下运行， 该代码专门针对php7及以上的php版本对xhprof做的修改。

## 编译安装
phpize
./configure --enable-xhprof
make
make install

## 使用和特性

```
$options = [
    //'ignored_functions' => ['bar'], //要忽略的函数
    'track_functions' => ['test', 'bar'], //要捕获的函数
];
xhprof_enable(0, $options);

//你的业务代码

$xhprof_data = xhprof_disable();

var_dump($xhprof_data);

```


