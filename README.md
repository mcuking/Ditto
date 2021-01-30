# Ditto
Ditto is a scripting language implemented in C

基于 C 语言实现一个脚本语言 Ditto

## 目的

本仓库目的是为了探索编程语言的底层运行机制，并不是实现一个用于实际开发的编程语言。

Ditto 编程语言几乎会完全按照[《自制编程语言 基于 C 语言》](https://book.douban.com/subject/30311070/)来实现，但会在书中代码的基础上，增加非常丰富的注释，方便读者更容易地理解编程语言的实现思路。

作者学习[《自制编程语言 基于 C 语言》](https://book.douban.com/subject/30311070/)的目的是为了后面从语言特性解读 JS 引擎 [QuickJS](https://github.com/bellard/quickjs) 作准备，后面计划撰写一本关于 [QuickJS](https://github.com/bellard/quickjs) 源码剖析的书，欢迎阅读。相关仓库地址如下，欢迎 Star：

[understand-quickjs](https://github.com/mcuking/understand-quickjs)

## 如何调试

本项目采用 make 进行构建，另外针对 VSCode 添加了相应的配置文件 launch.json 以及 tasks.json。

仅需要使用 VSCode 加载本项目，并且在代码中设置相应的断点，然后启动 debug 即可进行调试。

## 阶段成果

### 实现词法分析器

```
import people for People 
fun fn() {
   var p = People.new("xiaoming", "male", 20.0)
   p.sayHi()
}

class Family < People {
   var father
   var mother
   var child
   new(f, m, c) {
      father = f
      mother = m
      child  = c
      super("wbf", "male", 60)
   }
}

var f = Family.new("wbf", "ls", "shine")
f.sayHi()

fn()
```

对上面的源码串进行词法分析，生成 Token 流，并打印出字符串和 token 类型的映射关系：

```
1L: IMPORT [import]
1L: ID [people]
1L: FOR [for]
1L: ID [People]
2L: FUN [fun]
2L: ID [fn]
2L: LEFT_PAREN [(]
2L: RIGHT_PAREN [)]
2L: LEFT_BRACE [{]
3L: VAR [var]
3L: ID [p]
3L: ASSIGN [=]
3L: ID [People]
3L: DOT [.]
3L: ID [new]
3L: LEFT_PAREN [(]
3L: STRING ["xiaoming"]
3L: COMMA [,]
3L: STRING ["male"]
3L: COMMA [,]
3L: NUM [20.0]
3L: RIGHT_PAREN [)]
4L: ID [p]
4L: DOT [.]
4L: ID [sayHi]
4L: LEFT_PAREN [(]
4L: RIGHT_PAREN [)]
5L: RIGHT_BRACE [}]
7L: CLASS [class]
7L: ID [Family]
7L: LESS [<]
7L: ID [People]
7L: LEFT_BRACE [{]
8L: VAR [var]
8L: ID [father]
...
```
