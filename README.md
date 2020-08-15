# Ditto
Ditto is a scripting language implemented in C

基于 C 语言实现一个脚本语言 Ditto

## 目的

本仓库目的是为了探索编程语言的底层运行机制，并不是实现一个用于实际开发的编程语言。

计划初期按照[《自制编程语言 基于 C 语言》](https://book.douban.com/subject/30311070/)进行开发，后续会在此基础上做进一步定制开发。

其中几乎所有需要解释的代码都会有相关注释，方便理解整体实现思路。

## 阶段成果

### 实现初步词法分析器

```
import people for People 
fun fn() {
   var p = People.new("xiaoming", "male")
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
      super("wbf", "male")
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
