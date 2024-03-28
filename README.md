## 1.项目背景

对[soyersoyer/basefind2: A faster firmware base address scanner. (github.com)](https://github.com/soyersoyer/basefind2)这个工具进行了重写，原项目使用python编写，在进行大型固件的基地址查找时十分缓慢，因此为了加快分析速度，使用C语言进行了重构，同时添加了多线程来加快执行速度。

查找加载基地址的原理为：

**明确一个基本的定理：字符串在内存中的加载地址=固件的加载基地址+字符串在固件中的偏移量。**
**首先提取出固件中所有的字符串，将字符串在固件中的偏移量作为一个数组(字符串偏移量数组)，显然，这个数组是升序排列的；然后将固件中的每个4字节作为一个指针，按照升序排列的方式构成一个数组(指针数组)；**
**然后将字符串数组中的值进行两两相减，得到一个字符串偏移量差值数组，这个数组表明了两两字符串之间的相对偏移；**
**再对指针数组也做同样的处理得到一个指针差值数组，这个数组表明了指针之间的相对偏移；**
**在指针数组之中存在着一些字符串的绝对加载地址，如果存在一些绝对加载地址是两两相邻的情况，那么将指针数组处理得到指针差值数组之后，这些绝对加载地址之间的差值实际上也就是它所引用的字符串偏移量之间的差值，这些差值是可以在字符串偏移量差值数组中有所体现的。**
**所以，可以在指针差值数组之中按照一定的长度和字符串偏移量差值数组进行匹配，如果匹配到就用相应的指针减去字符串的偏移量得到一个可能的加载基地址，再将所有的字符串偏移量加上这个基地址得到一个确切的字符串加载地址，并在指针数组中查找这些字符串加载地址是否存在，每匹配到一次就将相应的加载基地址的匹配次数加一，最后输出次数排名前十或者指定排名的加载基地址**

工具的使用方式如下

```
$ ./basefind_multi_thread -h
Usage: ./basefind_multi_thread [-h] [--sl/-l STRLENGTH] [--dl/-d DIFFLENGTH] [--sr/-s SAMPLERATE] [--thread/-t thread_num(default 1)] [--output_num/-o output_num] [--file/-f filename]
```

-l指定要搜集的字符串的长度，默认为10，也就是搜集长度大于等于10的字符串的偏移量

-d指定在对指针差值数组和字符串偏移量差值数组进行匹配时，一次性匹配多长的字符串偏移量差值数组，默认值为10，也就是每次从指针差值数组中匹配10个单位的字符串偏移量差值数组

-s指定查找间隔，默认为20，在确定了一个可能的加载基地址之后，验证"加载基地址+字符串偏移量==指针"时,每次会跳过20个字符串。

-t指定要以多少个线程来进行分析，默认为1

-o指定最终会输出的可能的加载基地址的数量，默认为10个

## 2.性能测试

使用[ARM固件基址定位工具开发-安全客 - 安全资讯平台 (anquanke.com)](https://www.anquanke.com/post/id/198276#h2-12)验证工具的准确性

### 1.sony

![image-20240328132744224](https://raw.githubusercontent.com/LindLL/blog-img/main/202403281327711.png)

和原文中所得结果一致

### 2.iAudio 10 MP3固件

![image-20240328132904501](https://raw.githubusercontent.com/LindLL/blog-img/main/202403281329969.png)

![image-20240328132923189](https://raw.githubusercontent.com/LindLL/blog-img/main/202403281329879.png)

和原文中所得结果一致

### 3.opkg

使用默认参数

![image-20240328133231933](https://raw.githubusercontent.com/LindLL/blog-img/main/202403281332236.png)

![image-20240328133251404](https://raw.githubusercontent.com/LindLL/blog-img/main/202403281332992.png)

也可以得到到对应结果

### 4.websrv

这个程序是开启了pie的，固件加载地址都是针对于RTOS或者裸机程序而言

![image-20240328143812929](https://raw.githubusercontent.com/LindLL/blog-img/main/202403281438092.png)

### 5.佳能某打印机固件

这个固件有50多MB，非常巨大，修正一下参数

![image-20240328144457693](https://raw.githubusercontent.com/LindLL/blog-img/main/202403281444891.png)

![image-20240328144523938](https://raw.githubusercontent.com/LindLL/blog-img/main/202403281445714.png)

在跑的过程中可以明显看到有地址是出现了很多次的，因此这个地址具有高度的可疑性，后续带入验证也能证明这个地址是正确的。

