# Ditto

Ditto is a scripting language implemented in C

Ditto 是一个基于 C 语言实现的脚本语言

该语言完全按照 [《自制编程语言 基于 C 语言》](https://book.douban.com/subject/30311070/) 实现，另外基于笔者的理解添加了丰富的注释。后续会在现有基础上尝试更多地定制开发，目的是探索编程语言底层的运行机制。

笔者同时也在研读 JS 引擎 [QuickJS](https://github.com/bellard/quickjs) 的源码，希望能够解读 JS 语言特性在语言引擎内部的实现原理，加深对编程语言的理解，欢迎关注。

https://github.com/mcuking/understand-quickjs

## 源码目录

```shell
Ditto
├── CMakeLists.txt      CMake 构建配置文件
├── Makefile            Make 构建配置文件
├── README.md           介绍文档
├── cli                 引擎入口，可直接执行脚本代码或脚本代码文件
├── compiler            编译器部分，负责编译脚本代码并生成对应字节码
├── examples            示例脚本代码，可用于开发调试和测试
├── gc                  垃圾回收
├── include             公共方法
├── lexer               词法分析部分
├── object              脚本语言内置的数据结构和相应方法
└── vm
    ├── core.c              脚本语言内置的原生方法
    ├── core.h
    ├── core.script.inc     脚本语言内置的脚本方法
    ├── opcode.inc          字节码中全部的操作码的集合
    ├── vm.c                虚拟机部分，负责执行编译器生成的字节码
    └── vm.h
```

## 原理

该语言的实现主要包括两个部分：编译器和虚拟机。编译器会对待执行的脚本代码进行词法分析、语法分析、语义分析，生成字节码，然后交由虚拟机解码并执行字节码中的指令，以实现脚本代码的意图。

更详细的原理图待后续整理好上传。

## 构建

本项目支持 CMake 和 Make 两种方式构建，另外针对 VSCode 添加了相应的配置文件 launch.json 以及 tasks.json，方便进行 debug。推荐使用 CLion 或者 VSCode 运行项目，下面是 CLion Debug 相关的配置参数。

![image.png](https://i.loli.net/2021/02/16/tu4YT6kU7RhrXZs.png)

## 使用

本项目支持执行文件和命令行两种方式运行脚本语言。

### 执行文件

在运行可执行文件的命令中，添加需要执行的脚本代码的文件路径作为参数即可，如下：

```shell
./di ../exmaples/manager.di
```

### 命令行

运行可执行文件后，在命令行后输入脚本语言代码即可，输入 quit 退出命令行。(目前只支持输入单行代码)

```shell
./di
>>> const arr = ["a", "b", "c"]
>>> System.print(arr)
>>> arr.add("d")
>>> System.print(arr[arr.count - 1])
```

## TODO

- [ ] 实现垃圾回收功能

## 语言特性简介

该语言是完全面向对象的弱类型语言，数据类型包括 String、Bool、Number、Map、List、range 等，支持闭包、模块化、垃圾回收等特性。下面是代码示例，更多代码请阅读 examples 中的文件。

```
import employee for Employee
var xh =  Employee.new("xiaohong", "female", 20, 6000)
System.print(xh.salary)

var xm =  Employee.new("xiaoming", "male", 23, 8000)
System.print(xm.salary)

System.print(Employee.employeeNum)

class Manager < Employee {
   var bonus
   bonus=(v) {
      bonus = v
   }

   new(n, g, a, s, b) {
      super(n, g, a, s)
      bonus = b
   }

   salary {
      return super.salary + bonus
   }

}

fun employeeInfo() {
   System.print("number of employee:" + Employee.employeeNum.toString)
   var employeeTitle = Map.new()
   employeeTitle["xh"] = "rd"
   employeeTitle["xm"] = "op"
   employeeTitle["lw"] = "manager"
   employeeTitle["lz"] = "pm"

   for k (employeeTitle.keys) {
      System.print(k + " -> " + employeeTitle[k])
   }

   var employeeHeight = {
      "xh": 170,
      "xm": 172,
      "lw": 168,
      "lz": 173
   }
   var totalHeight = 0
   for v (employeeHeight.values) {
      totalHeight = totalHeight + v
   }
   System.print("averageHeight: %(totalHeight / employeeHeight.count)")

   var allEmployee = ["xh", "xm", "lw", "lz"]
   for e (allEmployee) {
      System.print(e)
   }

   allEmployee.add("xl")
   System.print("all employee are:%(allEmployee.toString)")
   var idx = 0
   var count = allEmployee.count
   while (idx < count) {
      System.print(allEmployee[idx])
      idx = idx + 1
   }

   // System.gc()  //可以手动回收内存

   var a = 3 + 5 > 9 - 3  ? "yes" : "no"
   if (a.endsWith("s")) {
      System.print(System.clock)
   } else {
      System.print("error!!!!!")
   }

   var str = "hello, world."
   System.print(str[-1..0])
}

var lw = Manager.new("laowang", "male", 35, 13000, 2000)
System.print(lw.salary)
lw.bonus=3100
System.print(lw.salary)
var lz = Manager.new("laozheng", "male", 36, 15000, 2300)
System.print(lz.salary)

var thread = Thread.new(employeeInfo)
thread.call()
```

