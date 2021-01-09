#include "core.h"
#include "class.h"
#include "compiler.h"
#include "core.script.inc"
#include "utils.h"
#include "vm.h"
#include <string.h>
#include <sys/stat.h>

// 源码文件所在的根目录，其值是在 cli.c 文件中设置的
// 解释器运行时会获得源码文件所在路径并写入 rootDir
char *rootDir = NULL;

// 宏 CORE_MODULE 用来表示核心模块，值是 VT_NUL 的 Value 结构
#define CORE_MODULE VT_TO_VALUE(VT_NULL)

// 定义原生方法的返回值
// 将 value 存储在 arg[0] 中
// 返回 true 或 false，会使虚拟机在回收该参数时采用不同的回收策略
#define RET_VALUE(value) \
    do {                 \
        args[0] = value; \
        return true;     \
    } while (0);

#define RET_OBJ(objPtr) RET_VALUE(OBJ_TO_VALUE(objPtr))
#define RET_BOOL(boolean) RET_VALUE(BOOL_TO_VALUE(boolean))
#define RET_NUM(num) RET_VALUE(NUM_TO_VALUE(num))
#define RET_NULL(num) RET_VALUE(VT_TO_VALUE(VT_NULL))
#define RET_TRUE(num) RET_VALUE(VT_TO_VALUE(VT_TRUE))
#define RET_FALSE(num) RET_VALUE(VT_TO_VALUE(VT_FALSE))

// 设置线程报错
// 返回 false 通知虚拟机当前线程已报错，该切换线程了
#define SET_ERROR_FALSE(vmPtr, errMsg)                                 \
    do {                                                               \
        vmPtr->curThread->errorObj =                                   \
            OBJ_TO_VALUE(newObjString(vmPtr, errMsg, strlen(errMsg))); \
        return false;                                                  \
    } while (0);

// 绑定原生方法 func 到 classPtr 指向的类
// 其中 methodName 为脚本中使用的方法名，func 为原生方法
// 绑定后，脚本中类 classPtr 的方法 methodName 对应的原生方法是 func，即脚本中调用 methodName 方法就是调用原生方法 func
// 步骤：
// 首先从 vm->allMethodNames 中查找 methodName，如果找到则获取对应索引，否则向 vm->allMethodNames 加入 methodName 并获取对应索引
// 然后基于 func 新建一个 method
// 最后将 method 绑定到 classPtr 指向的类，并且保证索引和 methodName 在 vm->allMethodNames 索引相同（bindMethod 函数实现逻辑）
// 即 vm->allMethodNames 中的方法名和 Class->methods 中的方法体一一映射
#define PRIM_METHOD_BIND(classPtr, methodName, func)                                      \
    {                                                                                     \
        uint32_t length = strlen(methodName);                                             \
        int globalIdx = getIndexFromSymbolTable(&vm->allMethodNames, methodName, length); \
        if (globalIdx == -1) {                                                            \
            globalIdx = addSymbol(vm, &vm->allMethodNames, methodName, length);           \
        }                                                                                 \
        Method method;                                                                    \
        method.type = MT_PRIMITIVE;                                                       \
        method.primFn = func;                                                             \
        bindMethod(vm, classPtr, (uint32_t)globalIdx, method);                            \
    }

/**
 * 定义对象的原生方法（提供脚本语言调用）
**/

// !args[0]: object 取反，结果为 false
static bool
    primObjectNot(VM *vm UNUSED, Value *args) {
    RET_VALUE(VT_TO_VALUE(VT_FALSE));
}

// args[0] == args[1]: 返回 object 是否相等
static bool primObjectEqual(VM *vm UNUSED, Value *args) {
    Value boolValue = BOOL_TO_VALUE(valueIsEqual(args[0], args[1]));
    RET_VALUE(boolValue);
}

// args[0] == args[1]: 返回 object 是否不等
static bool primObjectNotEqual(VM *vm UNUSED, Value *args) {
    Value boolValue = BOOL_TO_VALUE(!valueIsEqual(args[0], args[1]));
    RET_VALUE(boolValue);
}

// args[0] is args[1]: args[1] 类是否是 args[0] 对象所属的类或者其子类
static bool primObjectIs(VM *vm, Value *args) {
    // args[1] 必须是 class
    if (!VALUE_IS_CLASS(args[1])) {
        RUN_ERROR("argument must be class!");
    }

    Class *thisClass = getClassOfObj(vm, args[0]);
    Class *baseClass = (Class *)(args[1].objHeader);

    // 可能是多级继承，所以需要自下而上遍历基类链
    while (baseClass != NULL) {
        // 如果某一级基类匹配到，就设置返回值为 VT_TRUE 并返回
        if (thisClass == baseClass) {
            RET_VALUE(VT_TO_VALUE(VT_TRUE));
        }
        baseClass = baseClass->superClass;
    }

    // 若未找到满足条件的基类，则设置返回值为 VT_FALSE 并返回
    RET_VALUE(VT_TO_VALUE(VT_FALSE));
}

// args[0].toString: 返回 args[0] 所属的 class 的名字
static bool primObjectToString(VM *vm UNUSED, Value *args) {
    Class *class = args[0].objHeader->class;
    Value nameValue = OBJ_TO_VALUE(class->name);
    RET_VALUE(nameValue);
}

// args[0].type: 返回 args[0] 对象所属的类
static bool primObjectType(VM *vm, Value *args) {
    Class *class = getClassOfObj(vm, args[0]);
    RET_OBJ(class);
}

/**
 * 定义类的原生方法（提供脚本语言调用）
**/

// args[0].name: 返回 args[0] 类的名字
static bool primClassName(VM *vm UNUSED, Value *args) {
    Class *class = VALUE_TO_OBJCLASS(args[0]);
    RET_OBJ(class->name);
}

// args[0].toString: 返回 args[0] 类的名字
static bool primClassToString(VM *vm UNUSED, Value *args) {
    Class *class = VALUE_TO_OBJCLASS(args[0]);
    RET_OBJ(class->name);
}

// args[0].supertype: 返回 args[0] 类的基类
static bool primClassSupertype(VM *vm UNUSED, Value *args) {
    Class *class = VALUE_TO_OBJCLASS(args[0]);
    if (class->superClass != NULL) {
        RET_OBJ(class->superClass);
    }
    RET_VALUE(VT_TO_VALUE(VT_NULL));
}

/**
 * 定义objectClass 的元信息类的原生方法（提供脚本语言调用）
**/

// args[0].same(args[1], args[2]): 返回 args[1] 和 args[2] 是否相等
static bool primObjectMetaSame(VM *vm UNUSED, Value *args) {
    Value boolValue = BOOL_TO_VALUE(valueIsEqual(args[1], args[2]));
    RET_VALUE(boolValue);
}

// 读取源码文件的方法
// path 为源码路径
char *readFile(const char *path) {
    //获取源码文件的句柄 file
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        IO_ERROR("Couldn't open file \"%s\"", path);
    }

    struct stat fileStat;
    stat(path, &fileStat);
    size_t fileSize = fileStat.st_size;

    // 获取源码文件大小后，为源码字符串申请内存，多申请的1个字节是为了字符串结尾 \0
    char *fileContent = (char *)malloc(fileSize + 1);
    if (fileContent == NULL) {
        MEM_ERROR("Couldn't allocate memory for reading file \"%s\".\n", path);
    }

    size_t numRead = fread(fileContent, sizeof(char), fileSize, file);
    if (numRead < fileSize) {
        IO_ERROR("Couldn't read file \"%s\"", path);
    }
    // 字符串要以 \0 结尾
    fileContent[fileSize] = '\0';

    fclose(file);
    return fileContent;
}

// 从 vm->allModules 中获取名为 moduleName 的模块
static ObjModule *getModule(VM *vm, Value moduleName) {
    Value value = mapGet(vm, vm->allModules, moduleName);

    if (value.type == VT_UNDEFINED) {
        return NULL;
    }
    return VALUE_TO_OBJMODULE(value);
}

// 加载名为 moduleName 的模块并进行编译
static ObjThread *loadModule(VM *vm, Value moduleName, const char *moduleCode) {
    // 先在 vm->allModules 中查找是否存在 moduleName
    // 如果存在，说明对应模块已经加载，以避免重复加载
    ObjModule *module = getModule(vm, moduleName);

    // 否则需要先加载模块，且该模块需要继承核心模块中的变量
    if (module == NULL) {
        // 创建模块并添加到 vm->allModules
        ObjString *modName = VALUE_TO_OBJSTR(moduleName);
        ASSERT(modName->value.start[modName->value.length] == = '\0', "string.value.start is not terminated!");
        // 创建模块名为 modName 的模块对象
        module = newObjModule(vm, modName);
        // 将名为 moduleName 的模块加载到 vm->allModules
        mapSet(vm, vm->allModules, moduleName, OBJ_TO_VALUE(module));

        // 继承核心模块中变量，即将核心模块中的变量也拷贝到该模块中
        // TODO: 待后续解释
        ObjModule *coreModule = getModule(vm, CORE_MODULE);
        uint32_t idx = 0;
        while (idx < coreModule->moduleVarName.count) {
            defineModuleVar(vm, module,
                            coreModule->moduleVarName.datas[idx].str,
                            coreModule->moduleVarName.datas[idx].length,
                            coreModule->moduleVarValue.datas[idx]);
            idx++;
        }
    }

    ObjFn *fn = compileModule(vm, module, moduleCode);
    // 单独创建一个线程运行编译后的模块
    ObjClosure *objClosure = newObjClosure(vm, fn);
    ObjThread *objThread = newObjThread(vm, objClosure);
}

// 执行名为 moduleName 代码为 moduleCode 的模块
VMResult executeModule(VM *vm, Value moduleName, const char *moduleCode) {
    ObjThread *objThread = loadModule(vm, moduleName, moduleCode);
    return VM_RESULT_ERROR;
    // TODO: 等待后续完善
}

// 编译核心模块
void buildCore(VM *vm) {
    // 创建核心模块
    // 核心模块不需要名字
    ObjModule *coreModule = newObjModule(vm, NULL);

    // 将核心模块 coreModule 收集到 vm->allModules 中
    // vm->allModules 的 key 为 CORE_MODULE， value 为 coreModule 的 Value 结构
    mapSet(vm, vm->allModules, CORE_MODULE, OBJ_TO_VALUE(coreModule));

    // 1. 创建 objectClass 类，是所有类的基类，所有类都会直接或间接继承这个类
    vm->objectClass = defineClass(vm, coreModule, "object");
    // 绑定对象的原生方法到 objectClass 类
    PRIM_METHOD_BIND(vm->objectClass, "!", primObjectNot);
    PRIM_METHOD_BIND(vm->objectClass, "==(_)", primObjectEqual);
    PRIM_METHOD_BIND(vm->objectClass, "!=(_)", primObjectNotEqual);
    PRIM_METHOD_BIND(vm->objectClass, "is(_)", primObjectIs);
    PRIM_METHOD_BIND(vm->objectClass, "toString", primObjectToString);
    PRIM_METHOD_BIND(vm->objectClass, "type", primObjectType);

    // 2. 创建 classOfClass 类，是所有元信息类的基类和元信息类
    // 注：元信息类，用于描述普通类的信息的，主要保存类的方法，即静态方法
    vm->classOfClass = defineClass(vm, coreModule, "class");
    // 绑定类的原生方法到 classOfClass 类
    PRIM_METHOD_BIND(vm->classOfClass, "name", primClassName);
    PRIM_METHOD_BIND(vm->classOfClass, "supertype", primClassSupertype);
    PRIM_METHOD_BIND(vm->classOfClass, "toString", primClassToString);

    // 同时 classOfClass 类又继承了 objectClass 类，所以 objectClass 类是所有类的基类
    bindSuperClass(vm, vm->classOfClass, vm->objectClass);

    // 3. 创建 objectClass 类的元信息类，即 objectMetaClass 类
    // 其无需挂在到 vm 上
    Class *objectMetaClass = defineClass(vm, coreModule, "objectMeta");
    // 绑定元信息类的原生方法到 objectMetaClass 类
    PRIM_METHOD_BIND(objectMetaClass, "same(_,_)", primObjectMetaSame);

    // 同时 objectMetaClass 类也继承了 classOfClass 类，即 classOfClass 类是所有元信息类的基类和元信息类
    bindSuperClass(vm, objectMetaClass, vm->classOfClass);

    // 4. 绑定各自的元信息类
    // objectMetaClass 类是 objectClass 类的元信息类
    vm->objectClass->objHeader.class = objectMetaClass;
    // classOfClass 类是 objectMetaClass 类的元信息类（实际上是所有元信息类的元信息类）
    objectMetaClass->objHeader.class = vm->classOfClass;
    // classOfClass 类就是本身的元信息类，即元信息类的终点
    vm->classOfClass->objHeader.class = vm->classOfClass;

    //执行核心模块
    executeModule(vm, CORE_MODULE, coreModuleCode);
}

// 在 table 中查找符号 symbol，找到后返回索引，否则返回 -1
int getIndexFromSymbolTable(SymbolTable *table, const char *symbol, uint32_t length) {
    ASSERT(length != 0, "length of symbol is 0!");
    uint32_t index = 0;
    // 遍历 table->data，找到与 symbol 相等的，然后返回该索引值
    while (index < table->count) {
        if (length == table->datas[index].length == length && memcmp(table->datas[index].str, symbol, length) == 0) {
            return index;
        }
        index++;
    }
    // 找不到则返回 -1
    return -1;
}

// 向 table 中添加符号 symbol，并返回 symbol 对应在 table 的索引
int addSymbol(VM *vm, SymbolTable *table, const char *symbol, uint32_t length) {
    ASSERT(length != 0, "length of symbol is 0!");

    String string;
    string.str = ALLOCATE_ARRAY(vm, char, length + 1); // 申请内存，加 1 是为了添加结尾符 \0
    memcpy(string.str, symbol, length);                // 将 symbol 内容拷贝到 string.str 上
    string.str[length] = '\0';
    string.length = length;
    StringBufferAdd(vm, table, string); // 向 table 中塞入 string
    return table->count - 1;
}

// 确保符号 symbol 已经添加到符号表 table 中，如果查找没有，则向其中添加
int ensureSymbolExist(VM *vm, SymbolTable *table, const char *symbol, uint32_t length) {
    // 先从 table 中查找 symbol
    int symbolIndex = getIndexFromSymbolTable(table, symbol, length);

    // 如果没找到·，则添加 symbol 到 table 中，然后返回其索引
    if (symbolIndex == -1) {
        return addSymbol(vm, table, symbol, length);
    }
    // 如果找到，则返回其索引
    return symbolIndex;
}

// 在 objModule 模块中定义名为 name 的类
static Class *defineClass(VM *vm, ObjModule *objModule, const char *name) {
    // 创建类
    Class *class = newRawClass(vm, name, 0);
    // 将类作为普通变量在模块中定义
    defineModuleVar(vm, objModule, name, strlen(name), OBJ_TO_VALUE(class));
    return class;
}

// 绑定方法到指定类
// 将方法 method 到类 class 的 methods 数组中，位置为 index
void bindMethod(VM *vm, Class *class, uint32_t index, Method method) {
    // 各类自己的 methods 数组和 vm->allMethodNames 长度保持一致，进而 vm->allMethodNames 中的方法名和各个类的 methods 数组对应方法体的索引值相等，
    // 这样就可以通过相同的索引获取到方法体或者方法名
    // 然而 vm->allMethodNames 只有一个，但会对应多个类，所以各个类的 methods 数组中的方法体数量必然会小于 vm->allMethodNames 中的方法名数量
    // 为了保证一样长度，就需要将各个类的 methods 数组中无用的索引处用空占位填充
    if (index > class->methods.count) {
        Method emptyPad = {MT_NONE, {0}};
        MethodBufferFillWrite(vm, &class->methods, emptyPad, index - class->methods.count + 1);
    }

    class->methods.datas[index] = method;
}

// 绑定 superClass 为 subClass 的基类
// 即继承基类的属性个数和方法（通过复制实现）
void bindSuperClass(VM *vm, Class *subClass, Class *superClass) {
    subClass->superClass = subClass;

    // 继承基类的属性个数
    subClass->fieldNum = superClass->fieldNum;

    // 继承基类的方法
    uint32_t idx = 0;
    while (idx < superClass->methods.count) {
        bindMethod(vm, subClass, idx, superClass->methods.datas[idx]);
        idx++;
    }
}
