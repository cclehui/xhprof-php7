# xhprof-php7
facebook开源的xhprof 扩展不支持Php7， 只能在php5及以下运行。

因为xhprof抓取的内容过多，而且计算了调用链，但是这种东西对实际生产没有什么帮助，故很少有吧xhprof部署在生产环境使用的

本项目提供只抓取指定函数的能力，而且可以部署在生产环境使用，性能损耗很低:每千万次函数抓取耗时在0.25s

## 编译安装
phpize
./configure --enable-xhprof
make
make install

## 使用和特性

```
$options = [
    'track_functions' => ['test', 'bar', 'TempUtil:test'], //要捕获的函数
];
//xhprof_enable(XHPROF_ALGORITHM_HASH, $options); //hash 查找方式
xhprof_enable(XHPROF_ALGORITHM_TRIE, $options); //字典树查找方式

//你的业务代码

$xhprof_data = xhprof_disable();

var_dump($xhprof_data);

```


