#include "core.h"
#include "class.h"
#include "compiler.h"
#include "core.script.inc"
#include "unicodeUtf8.h"
#include "utils.h"
#include "vm.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// 源码文件所在的根目录，其值是在 cli.c 文件中设置的
// 解释器运行时会获得源码文件所在路径并写入 rootDir
char *rootDir = NULL;

// 宏 CORE_MODULE 用来表示核心模块，值是 VT_NUL 的 Value 结构
#define CORE_MODULE VT_TO_VALUE(VT_NULL)

// 设置线程报错
// 返回 false 通知虚拟机当前线程已报错，该切换线程了
#define SET_ERROR_FALSE(vmPtr, errMsg)                                 \
    do {                                                               \
        vmPtr->curThread->errorObj =                                   \
            OBJ_TO_VALUE(newObjString(vmPtr, errMsg, strlen(errMsg))); \
        return false;                                                  \
    } while (0);

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

// 将数字转换为字符串
static ObjString *num2str(VM *vm, double num) {
    // NaN 不是一个确定的值,因此 NaN 和 NaN 是不相等的
    if (num != num) {
        return newObjString(vm, "NaN", 3);
    }

    if (num == INFINITY) {
        return newObjString(vm, "infinity", 8);
    }

    if (num == -INFINITY) {
        return newObjString(vm, "-infinity", 9);
    }

    // 以下 24 字节的缓冲区足以容纳双精度到字符串的转换
    char buf[24] = {'\0'};
    int len = sprintf(buf, "%.14g", num);
    return newObjString(vm, buf, len);
}

// 判断 arg 是否为字符串
static bool validateString(VM *vm, Value arg) {
    if (VALUE_IS_OBJSTR(arg)) {
        return true;
    }
    SET_ERROR_FALSE(vm, "argument must be string!");
}

// 判断 arg 是否为函数
static bool validateFn(VM *vm, Value arg) {
    if (VALUE_TO_OBJCLOSURE(arg)) {
        return true;
    }
    vm->curThread->errorObj = OBJ_TO_VALUE(newObjString(vm, "argument must be a function!", 28));
    return false;
}

//判断 arg 是否为数字
static bool validateNum(VM *vm, Value arg) {
    if (VALUE_IS_NUM(arg)) {
        return true;
    }
    SET_ERROR_FALSE(vm, "argument must be number!");
}

// 判断 value 是否为整数
static bool validateIntValue(VM *vm, double value) {
    if (trunc(value) == value) {
        return true;
    }
    SET_ERROR_FALSE(vm, "argument must be integer!");
}

// 判断 arg 是否为整数
static bool validateInt(VM *vm, Value arg) {
    // 首先得是数字
    if (!validateNum(vm, arg)) {
        return false;
    }

    // 再校验数值
    return validateIntValue(vm, VALUE_TO_NUM(arg));
}

// 判断参数 index 是否是落在 [0, length) 之间的整数
static uint32_t validateIndexValue(VM *vm, double index, uint32_t length) {
    // 索引必须是整数，如果校验失败则返回 UINT32_MAX
    // UINT32_MAX 是 32 为无符号数的最大值，即 5294967295，用十六进制表示就是 0xFFFFFFFF
    if (!validateIntValue(vm, index)) {
        return UINT32_MAX;
    }

    // 支持负数索引，负数是从后往前索引，转换其对应的正数索引
    if (index < 0) {
        index += length;
    }

    // 索引应该落在 [0, length)
    if (index >= 0 && index < length) {
        return (uint32_t)index;
    }

    // 执行到此说明超出范围
    vm->curThread->errorObj = OBJ_TO_VALUE(newObjString(vm, "index out of bound!", 19));
    return UINT32_MAX;
}

// 校验 index 合法性
static uint32_t validateIndex(VM *vm, Value index, uint32_t length) {
    return validateIndexValue(vm, VALUE_TO_NUM(index), length);
}

// 校验 key 合法性
static bool validateKey(VM *vm, Value arg) {
    if (VALUE_IS_TRUE(arg) ||
        VALUE_IS_FALSE(arg) ||
        VALUE_IS_NULL(arg) ||
        VALUE_IS_NUM(arg) ||
        VALUE_IS_OBJSTR(arg) ||
        VALUE_IS_OBJRANGE(arg) ||
        VALUE_IS_CLASS(arg)) {
        return true;
    }
    SET_ERROR_FALSE(vm, "key must be value type!");
}

// 基于码点 value 创建字符串
static Value makeStringFromCodePoint(VM *vm, int value) {
    uint32_t byteNum = getByteNumOfEncodeUtf8(value);
    ASSERT(byteNum != 0, "utf8 encode bytes should be between 1 and 4!");

    // +1是为了结尾的 '\0'
    ObjString *objString = ALLOCATE_EXTRA(vm, ObjString, byteNum + 1);

    if (objString == NULL) {
        MEM_ERROR("allocate memory failed in runtime!");
    }

    initObjHeader(vm, &objString->objHeader, OT_STRING, vm->stringClass);
    objString->value.length = byteNum;
    objString->value.start[byteNum] = '\0';
    encodeUtf8((uint8_t *)objString->value.start, value);

    // 根据字符串对象中的值 objString->value 设置对应的哈希值给 objString->hashCode
    hashObjString(objString);

    return OBJ_TO_VALUE(objString);
}

// 用索引 index 处的字符创建字符串对象
static Value stringCodePointAt(VM *vm, ObjString *objString, uint32_t index) {
    ASSERT(index < objString->value.length, "index out of bound!");
    int codePoint = decodeUtf8((uint8_t *)objString->value.start + index, objString->value.length - index);

    // 若不是有效的 utf8 序列，将其处理为单个裸字符
    if (codePoint == -1) {
        return OBJ_TO_VALUE(newObjString(vm, &objString->value.start[index], 1));
    }

    return makeStringFromCodePoint(vm, codePoint);
}

// 计算 objRange 中元素的起始索引及索引方向
// countPtr 指针指向存储 objRange 所能索引的元素个数的变量
// directionPtr 指针指向存储 objRange 索引方向的变量（-1 表示反向，索引递减；1 表示正向，索引递增）
static uint32_t calculateRange(VM *vm, ObjRange *objRange, uint32_t *countPtr, int *directionPtr) {
    uint32_t from = validateIndexValue(vm, objRange->from, *countPtr);
    if (from == UINT32_MAX) {
        return UINT32_MAX;
    }

    uint32_t to = validateIndexValue(vm, objRange->to, *countPtr);
    if (to == UINT32_MAX) {
        return UINT32_MAX;
    }

    //如果 from 和 to 为负值,经过 validateIndexValue 已经变成了相应的正索引
    // -1 表示反向，索引递减；1 表示正向，索引递增
    *directionPtr = from < to ? 1 : -1;
    // countPtr 指针指向存储 objRange 所能索引的元素个数的变量
    *countPtr = abs((int)(from - to)) + 1;
    return from;
}

// 按照 UTF-8 编码【从 sourceStr 中起始为 startIndex，方向为 direction 的 count 个字符】
static ObjString *newObjStringFromSub(VM *vm, ObjString *sourceStr, int startIndex, uint32_t count, int direction) {
    uint8_t *source = (uint8_t *)sourceStr->value.start;
    uint32_t totalLength = 0, idx = 0;

    // 计算没有 UTF-8 编码的字符的 UTF-8 编码字节数，以便后面申请内存空间
    while (idx < count) {
        totalLength += getByteNumOfDecodeUtf8(source[startIndex + idx * direction]);
        idx++;
    }

    // +1 是为了结尾的 '\0'
    ObjString *result = ALLOCATE_EXTRA(vm, ObjString, totalLength + 1);

    if (result == NULL) {
        MEM_ERROR("allocate memory failed in runtime!");
    }
    initObjHeader(vm, &result->objHeader, OT_STRING, vm->stringClass);
    result->value.start[totalLength] = '\0';
    result->value.length = totalLength;

    uint8_t *dest = (uint8_t *)result->value.start;
    idx = 0;
    while (idx < count) {
        int index = startIndex + idx * direction;
        // 先调用 decodeUtf8 获得字符的码点
        int codePoint = decodeUtf8(source + index, sourceStr->value.length - index);
        if (codePoint != -1) {
            // 然后调用 encodeUtf8 将码点按照 UTF-8 编码，并写入dest 即 result
            dest += encodeUtf8(dest, codePoint);
        }
        idx++;
    }

    // 根据字符串对象中的值 result->value 设置对应的哈希值给 result->hashCode
    hashObjString(result);
    return result;
}

// 使用 Boyer-Moore-Horspool 字符串匹配算法在 haystack 中查找 needle
static int findString(ObjString *haystack, ObjString *needle) {
    // 如果待查找的 patten 为空则为找到，直接返回 0 即可
    if (needle->value.length == 0) {
        //返回起始下标 0
        return 0;
    }

    // 若待搜索的字符串比原串还长，肯定搜不到，直接返回 -1 即可
    if (needle->value.length > haystack->value.length) {
        return -1;
    }

    // 构建 “bad-character shift表” 以确定窗口滑动的距离
    // 数组 shift 的值便是滑动距离
    uint32_t shift[UINT8_MAX];
    // needle 中最后一个字符的下标
    uint32_t needleEnd = needle->value.length - 1;

    // 一、先假定 “bad character” 不属于 needle(即 pattern)
    // 对于这种情况，滑动窗口跨过整个 needle
    uint32_t idx = 0;
    while (idx < UINT8_MAX) {
        // 默认为滑过整个 needle 的长度
        shift[idx] = needle->value.length;
        idx++;
    }

    // 二、假定 haystack 中与 needle 不匹配的字符在 needle 中之前已匹配过的位置出现过
    // 就滑动窗口以使该字符与在needle中匹配该字符的最末位置对齐。
    // 这里预先确定需要滑动的距离
    idx = 0;
    while (idx < needleEnd) {
        char c = needle->value.start[idx];
        // idx 从前往后遍历 needle，当 needle 中有重复的字符 c 时，
        // 后面的字符 c 会覆盖前面的同名字符 c，这保证了数组 shilf 中字符是 needle 中最末位置的字符，
        // 从而保证了 shilf[c] 的值是 needle中 最末端同名字符与 needle 末端的偏移量
        shift[(uint8_t)c] = needleEnd - idx;
        idx++;
    }

    // Boyer-Moore-Horspool 是从后往前比较，这是处理 bad-character 高效的地方，
    // 因此获取 needle 中最后一个字符，用于同 haystack 的窗口中最后一个字符比较
    char lastChar = needle->value.start[needleEnd];

    // 长度差便是滑动窗口的滑动范围
    uint32_t range = haystack->value.length - needle->value.length;

    // 从 haystack 中扫描 needle，寻找第 1 个匹配的字符，如果遍历完了就停止
    idx = 0;
    while (idx <= range) {
        // 拿 needle 中最后一个字符同 haystack 窗口的最后一个字符比较
        //（因为Boyer-Moore-Horspool是从后往前比较），如果匹配，看整个 needle 是否匹配
        char c = haystack->value.start[idx + needleEnd];
        if (lastChar == c &&
            memcmp(haystack->value.start + idx, needle->value.start, needleEnd) == 0) {
            // 找到了就返回匹配的位置
            return idx;
        }

        // 否则就向前滑动继续下一伦比较
        idx += shift[(uint8_t)c];
    }

    // 未找到就返回 -1
    return -1;
}

// 根据模块名获取文件绝对路径
// 拼接规则：rootDir + modileName + '.di'
static char *getFilePath(const char *moduleName) {
    uint32_t rootDirLength = rootDir == NULL ? 0 : strlen(rootDir);
    uint32_t nameLength = strlen(moduleName);
    uint32_t pathLength = rootDirLength + nameLength + strlen(".di");
    char *path = (char *)malloc(pathLength + 1);

    if (rootDir != NULL) {
        memmove(path, rootDir, rootDirLength);
    }

    memmove(path + rootDirLength, moduleName, nameLength);
    memmove(path + rootDirLength + nameLength, ".di", 3);
    path[pathLength] = '\0';
    return path;
}

// 读取名为 moduleName 的模块
static char *readModule(const char *moduleName) {
    char *modulePath = getFilePath(moduleName);
    char *moduleCode = readFile(modulePath);
    free(modulePath);
    return moduleCode;
}

// 输出字符串
static void printString(const char *str) {
    printf("%s", str);
    // 输出到缓冲区后立即刷新
    fflush(stdout);
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

// 从 vm->allModules 中获取名为 moduleName 的模块
static ObjModule *getModule(VM *vm, Value moduleName) {
    Value value = mapGet(vm->allModules, moduleName);

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
        ASSERT(modName->value.start[modName->value.length] == '\0', "string.value.start is not terminated!");
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

// 导入模块 moduleName，主要是把编译模块并加载到 vm->allModules
static Value importModule(VM *vm, Value moduleName) {
    // 若模块已经导入则返回 NULL
    if (!VALUE_IS_UNDEFINED(mapGet(vm->allModules, moduleName))) {
        return VT_TO_VALUE(VT_NULL);
    }
    ObjString *objString = VALUE_TO_OBJSTR(moduleName);
    // 读取名为 moduleName 的模块
    const char *sourceCode = readModule(objString->value.start);

    // 加载名为 moduleName 的模块并进行编译
    ObjThread *moduleThread = loadModule(vm, moduleName, sourceCode);
    return OBJ_TO_VALUE(moduleThread);
}

// 从模块 moduleName 中获取模块变量 variableName
static Value getModuleVariable(VM *vm, Value moduleName, Value variableName) {
    // 调用本函数前模块应该提前被加载
    // 也就是导入模块变量之前需要导入模块，在执行本函数之前，必须先执行 importModule 函数将整个模块加载进来
    // 所以编译 “import 模块 for 模块变量” 会先生成调用 importModule 函数的指令，再生成调用 getModuleVariable 函数的指令获取模块中某个模块变量
    ObjModule *objModule = getModule(vm, moduleName);

    // 如果模块没有被提前加载，则向 vm->curThread->errorObj 添加错误信息并返回 NULL
    if (objModule == NULL) {
        ObjString *modName = VALUE_TO_OBJSTR(moduleName);
        // 24 是下面 sprintf 中 fmt 中除 %s 的字符个数
        ASSERT(modName->value.length < 512 - 24, "id`s buffer not big enough!");
        char id[512] = {'\0'};
        int len = sprintf(id, "module \'%s\' is not loaded!", modName->value.start);
        vm->curThread->errorObj = OBJ_TO_VALUE(newObjString(vm, id, len));
        return VT_TO_VALUE(VT_NULL);
    }

    ObjString *varName = VALUE_TO_OBJSTR(variableName);

    // 从 objModule->moduleVarName 中获得待导入的模块变量的索引
    int index = getIndexFromSymbolTable(&objModule->moduleVarName, varName->value.start, varName->value.length);

    // 如果索引为 -1，即模块变量 variableName 不存在，则向 vm->curThread->errorObj 添加错误信息并返回 NULL
    if (index == -1) {
        // 32 是下面 sprintf 中 fmt 中除 %s 的字符个数
        ASSERT(varName->value.length < 512 - 32, "id`s buffer not big enough!");
        ObjString *modName = VALUE_TO_OBJSTR(moduleName);
        char id[512] = {'\0'};
        int len = sprintf(id, "variable \'%s\' is not in module \'%s\'!", varName->value.start, modName->value.start);
        vm->curThread->errorObj = OBJ_TO_VALUE(newObjString(vm, id, len));
        return VT_TO_VALUE(VT_NULL);
    }

    // 否则模块变量存在，直接返回对应的模块变量 variableName 的值
    return objModule->moduleVarValue.datas[index];
}

// 从核心模块中获取名为 name 的类
static Value getCoreClassValue(ObjModule *objModule, const char *name) {
    int index = getIndexFromSymbolTable(&objModule->moduleVarName, name, strlen(name));
    if (index == -1) {
        char id[MAX_ID_LEN] = {'\0'};
        memcpy(id, name, strlen(name));
        RUN_ERROR("something wrong occur: missing core class \"%s\"!", id);
    }
    return objModule->moduleVarValue.datas[index];
}

// 执行名为 moduleName 代码为 moduleCode 的模块
VMResult executeModule(VM *vm, Value moduleName, const char *moduleCode) {
    ObjThread *objThread = loadModule(vm, moduleName, moduleCode);
    return executeInstruction(vm, objThread);
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

//绑定 fn.call 的重载，同样一个函数的 call 方法支持 0～16 个参数
static void bindFnOverloadCall(VM *vm, const char *sign) {
    uint32_t index = ensureSymbolExist(vm, &vm->allMethodNames, sign, strlen(sign));
    //构造 method
    Method method = {MT_FN_CALL, {0}};
    bindMethod(vm, vm->fnClass, index, method);
}

// 定义原生方法的返回值
// 将 value 存储在 arg[0] 中
// 返回 true 或 false，会使虚拟机在回收该参数时采用不同的回收策略
#define RET_VALUE(value) \
    do {                 \
        args[0] = value; \
        return true;     \
    } while (0);

//将值转为 Value 格式后做为返回值
#define RET_OBJ(objPtr) RET_VALUE(OBJ_TO_VALUE(objPtr))
#define RET_BOOL(boolean) RET_VALUE(BOOL_TO_VALUE(boolean))
#define RET_NUM(num) RET_VALUE(NUM_TO_VALUE(num))
#define RET_NULL RET_VALUE(VT_TO_VALUE(VT_NULL))
#define RET_TRUE RET_VALUE(VT_TO_VALUE(VT_TRUE))
#define RET_FALSE RET_VALUE(VT_TO_VALUE(VT_FALSE))

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
 * Object 类的原生方法
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
 * Class 类的原生方法
**/

// args[0].name: 返回 args[0] 类的名字
static bool primClassName(VM *vm UNUSED, Value *args) {
    Class *class = VALUE_TO_CLASS(args[0]);
    RET_OBJ(class->name);
}

// args[0].toString: 返回 args[0] 类的名字
static bool primClassToString(VM *vm UNUSED, Value *args) {
    Class *class = VALUE_TO_CLASS(args[0]);
    RET_OBJ(class->name);
}

// args[0].supertype: 返回 args[0] 类的基类
static bool primClassSupertype(VM *vm UNUSED, Value *args) {
    Class *class = VALUE_TO_CLASS(args[0]);
    if (class->superClass != NULL) {
        RET_OBJ(class->superClass);
    }
    RET_VALUE(VT_TO_VALUE(VT_NULL));
}

/**
 * objectClass 的元信息类的原生方法
**/

// args[0].same(args[1], args[2]): 返回 args[1] 和 args[2] 是否相等
static bool primObjectMetaSame(VM *vm UNUSED, Value *args) {
    Value boolValue = BOOL_TO_VALUE(valueIsEqual(args[1], args[2]));
    RET_VALUE(boolValue);
}

/**
 * Bool 类的原生方法
**/

// args[0].toString: 返回 bool 的字符串形式
static bool primBoolToString(VM *vm, Value *args) {
    ObjString *objString;
    if (VALUE_TO_BOOL(args[0])) {
        objString = newObjString(vm, "true", 4);
    } else {
        objString = newObjString(vm, "false", 5);
    }
    RET_OBJ(objString);
}

// !args[0]： bool 值取反
static bool primBoolNot(VM *vm UNUSED, Value *args) {
    bool value = !VALUE_TO_BOOL(args[0]);
    RET_BOOL(value);
}

/**
 * Thread 类的原生方法
**/

// Thread.new(func): 创建一个 thread 实例
// 该方法是脚本中调用 Thread.new(func) 所执行的原生方法
static bool primThreadNew(VM *vm, Value *args) {
    // 参数必须为函数
    if (!validateFn(vm, args[1])) {
        return false;
    }

    ObjThread *objThread = newObjThread(vm, VALUE_TO_OBJCLOSURE(args[0]));

    // 使stack[0]为接收者,保持栈平衡
    objThread->stack[0] = VT_TO_VALUE(VT_NULL);
    objThread->esp++;
    RET_OBJ(objThread);
}

// Thread.abort(err): 以错误信息err为参数退出线程
// 该方法是脚本中调用 Thread.abort(err) 所执行的原生方法，所以 args[1] 便是 err
static bool primThreadAbort(VM *vm, Value *args) {
    // 保存 err 到线程的 errorObj 上
    vm->curThread->errorObj = args[1]; //
    // 在原生方法返回后，虚拟机应该判断 err 即 args[1] 是否为空，如果非空说明原生方法出现异常，然后进行异常捕获（该逻辑暂未实现）
    return VALUE_IS_NULL(args[1]);
}

// Thread.current: 返回当前的线程
// 该方法是脚本中调用 Thread.current 所执行的原生方法
static bool primThreadCurrent(VM *vm, Value *args UNUSED) {
    RET_OBJ(vm->curThread);
}

// Thread.suspend(): 挂起线程，退出解释器
// 该方法是脚本中调用 Thread.suspend() 所执行的原生方法
static bool primThreadSuspend(VM *vm, Value *args UNUSED) {
    // 将 curThread 设置为 NULL后，虚拟机监测到 curThread 为 NULL 是，表示当前没有可运行的线程，会退出解释器
    vm->curThread = NULL;
    return false;
}

// Thread.yield(arg): 带参数（args[1]）让出 CPU 使用权
// 该方法是脚本中调用 Thread.yield(arg) 所执行的原生方法
static bool primThreadYieldWithArg(VM *vm, Value *args) {
    // 交回 CPU 控制权给调用当前线程的那个线程
    ObjThread *curThread = vm->curThread;
    vm->curThread = curThread->caller;
    // 将当前线程的主调方设置为 NULL，断开与主调用的调用关系
    curThread->caller = NULL;

    // 若主调方不为空，即当前线程有主调方，
    if (vm->curThread != NULL) {
        // 如果当前线程有主调方，就将当前线程的返回值放在主调方的栈顶，使得主调方可以获得当前线程 yield 时的参数
        vm->curThread->esp[-1] = args[1];

        // 对于脚本来说，当前方法调用 Thread.yield(arg) 拥有两个参数，即 Thread 和 arg，那么这两个参数占用了栈中两个 slot，
        // 由于存储主调方的参数只需要一个 slot，所以丢弃栈顶空间，预留之前的次栈顶空间
        // 即丢弃 Thread.yield(arg) 中的 arg，也就是 args[1]，只保留 args[0] 用于存储主调用方线程通过 call 方法传递的参数
        // 即保留 thread 参数所在的空间,将来唤醒时用于存储 yield 结果
        curThread->esp--;
    }
    // 返回 false 给虚拟机，表示需要切换线程
    // 虚拟机会从 vm->curThread 中获取当前运行的线程，在函数开头已经将 vm->curThread 设置成当前线程的主调方
    // 如果 vm->curThread 为 NULL，虚拟机就认为全部线程执行完毕，可以退出了
    return false;
}

// Thread.yield(): 无参数让出 CPU 使用权
static bool primThreadYieldWithoutArg(VM *vm, Value *args UNUSED) {
    // 交回 CPU 控制权给调用当前线程的那个线程
    ObjThread *curThread = vm->curThread;
    vm->curThread = curThread->caller;
    // 将当前线程的主调方设置为 NULL，断开与主调用的调用关系
    curThread->caller = NULL;

    if (vm->curThread != NULL) {
        // 为保持通用的栈结构,如果当前线程有主调方，就将空值做为返回值放在主调方的栈顶
        vm->curThread->esp[-1] = VT_TO_VALUE(VT_NULL);
    }
    // 返回 false 给虚拟机，表示需要切换线程
    // 虚拟机会从 vm->curThread 中获取当前运行的线程，在函数开头已经将 vm->curThread 设置成当前线程的主调方
    // 如果 vm->curThread 为 NULL，虚拟机就认为全部线程执行完毕，可以退出了
    return false;
}

// 切换到下一个线程 nextThread
static bool switchThread(VM *vm, ObjThread *nextThread, Value *args, bool withArg) {
    // 在下一线程 nextThread 执行之前,其主调线程应该为空
    if (nextThread->caller != NULL) {
        RUN_ERROR("thread has been called!");
    }

    // 将下一个线程 nextThread 指向当前线程
    nextThread->caller = vm->curThread;

    // 只有已经运行完毕的线程 thread 的 usedFrameNum 才为 0，这种没有执行任务的线程不应该被调用
    if (nextThread->usedFrameNum == 0) {
        SET_ERROR_FALSE(vm, "a finished thread can`t be switched to!");
    }

    // 如果线程上次运行已经出错了，其 errorObj 就会记录出错对象，已经出错的线程不应该被调用
    if (!VALUE_IS_NULL(nextThread->errorObj)) {
        SET_ERROR_FALSE(vm, "a aborted thread can`t be switched to!");
    }

    // 背景知识：
    // 尽管待运行线程 nextThread 能运行是调用了该线程对象的 call 方法，但是 call 方法的参数可是存储都在当前线程 curThread 的栈中
    // 确切的说是存储在 curThread 中当前正在运行的闭包的运行时栈中
    // 当前线程 curThread 正要交出 CPU 使用权给 nextThread，待将来 nextThread 运行完毕后，再将 CPU 使用权交还给主调方，即 curThread
    // 则 curThread 恢复运行，curThread 需要从栈顶获取此时所调用的 nextThread 线程的返回值，所以我们需要将 nextThread 的返回值存储到
    // 主调方即当前线程 curThread 的栈顶，如此 curThread 才能拿到 nextThread 的返回值

    // 如果主调方是通过 nextThread.call() 无参数的形式激活 nextThread 运行，那么主调方栈中只有一个参数即 nextThread，此时正好用
    // nextThread 在栈中的 slot 来存储将来 nextThread 运行后的返回值
    // 如果主调方是通过 nextThread.call(arg) 有参数的形式激活 nextThread 运行，那么主调方栈中有一两个参数，分别是 nextThread 和 arg
    // 占用两个 slot，由于当前主调线程只需要一个 slot 来存储 nextThread 运行后的返回值，所以需要回收一个 slot 空间，
    // 只保留次栈顶用于存储 nextThread 运行后的返回值
    if (withArg) {
        vm->curThread->esp--;
    }

    ASSERT(nextThread->esp > nextThread->stack, "esp should be greater than stack!");

    // 线程 nextThread 如果之前通过 yield 让出了 CUP 使用权给主调方 curThread，
    // 这次主调方 curThread 又通过 nextThread.call(arg) 使 nextThread 恢复运行
    // 那么 nextThread.call(arg) 中的 arg 将作为返回值存储到 nextThread 的栈顶
    nextThread->esp[-1] = withArg ? args[1] : VT_TO_VALUE(VT_NULL);

    // 虚拟机是根据 vm->curThread 来确定当前运行的线程，设置当前线程为 nextThread
    vm->curThread = nextThread;

    // 返回 false 使虚拟机进行线程切换
    return false;
}

// objThread.call(arg): 有参数调用下一个线程 nextThread
// 该方法是脚本中调用 objThread.call(arg) 所执行的原生方法，注意是实例方法不是类方法
// 对于脚本来说，当前方法调用 Thread.call(arg) 拥有两个参数，即 Thread 和 arg，分别是 args[0] 和 args[1]
static bool primThreadCallWithArg(VM *vm, Value *args) {
    return switchThread(vm, VALUE_TO_OBJTHREAD(args[0]), args, true);
}

// objThread.call(): 无参数调用下一个线程 nextThread
// 该方法是脚本中调用 objThread.call() 所执行的原生方法，注意是实例方法不是类方法
// 对于脚本来说，当前方法调用 Thread.call() 拥有一个参数，即 Thread，也就是 args[0]
static bool primThreadCallWithoutArg(VM *vm, Value *args) {
    return switchThread(vm, VALUE_TO_OBJTHREAD(args[0]), args, false);
}

// objThread.isDone：返回线程是否运行完成
// 该方法是脚本中调用 objThread.isDone 所执行的原生方法，注意是实例方法不是类方法
static bool primThreadIsDone(VM *vm UNUSED, Value *args) {
    // 对于脚本来说，当前方法调用 Thread.isDone 拥有一个参数，即 Thread，也就是 args[0]，isDone 的调用者
    ObjThread *objThread = VALUE_TO_OBJTHREAD(args[0]);
    // 线程运行完毕的两种情况：
    // 1. 帧栈使用量 usedFramNum 为 0
    // 2. 线程是否有错误出现
    RET_BOOL(objThread->usedFrameNum == 0 || !VALUE_IS_NULL(objThread->errorObj));
}

/**
 * Fn 类的原生方法
**/

// 新建一个函数对象
// 该方法是脚本中调用 Fn.new(_) 所执行的原生方法
static bool primFnNew(VM *vm, Value *args) {
    // 代码块为参数必为闭包
    if (!validateFn(vm, args[1]))
        return false;

    // 直接返回函数闭包
    RET_VALUE(args[1]);
}

/**
 * Null 类的原生方法
**/

// !null: null 取非得到 true
// 该方法为 Null 类的实例方法
static bool primNullNot(VM *vm UNUSED, Value *args UNUSED) {
    RET_VALUE(BOOL_TO_VALUE(true));
}

// null.toString: null 字符串化
// 该方法为 Null 类的实例方法
static bool primNullToString(VM *vm, Value *args UNUSED) {
    ObjString *objString = newObjString(vm, "null", 4);
    RET_OBJ(objString);
}

/**
 * Num 类的原生方法
**/

// 将字符串转换为数字
// 该方法是脚本中调用 Num.fromString(_) 所执行的原生方法，为类的方法
static bool primNumFromString(VM *vm, Value *args) {
    if (!validateString(vm, args[1])) {
        return false;
    }

    ObjString *objString = VALUE_TO_OBJSTR(args[1]);

    // 空字符串返回 RETURN_NULL
    if (objString->value.length == 0) {
        RET_NULL;
    }

    ASSERT(objString->value.start[objString->value.length] == '\0', "objString don`t teminate!");

    errno = 0;
    char *endPtr;

    // 将字符串转换为 double 型, 它会自动跳过前面的空白
    double num = strtod(objString->value.start, &endPtr);

    // 以 endPtr 是否等于 start+length 来判断不能转换的字符之后是否全是空白
    while (*endPtr != '\0' && isspace((unsigned char)*endPtr)) {
        endPtr++;
    }

    if (errno == ERANGE) {
        RUN_ERROR("string too large!");
    }

    // 如果字符串中不能转换的字符不全是空白，则字符串非法，返回 NULL
    if (endPtr < objString->value.start + objString->value.length) {
        RET_NULL;
    }

    // 至此，检查通过，返回正确结果
    RET_NUM(num);
}

// 返回圆周率
// 该方法是脚本中调用 Num.pi 所执行的原生方法，为类的方法
static bool primNumPi(VM *vm UNUSED, Value *args UNUSED) {
    RET_NUM(3.14159265358979323846);
}

// 定义 Num 相关中缀运算符的宏，共性如下：
// 先校验数字的合法性，然后再用 args[0] 和 args[1] 做中追运算符表示的运算
// 例如 1 + 2，args[0] 是 1，args[1] 是 2，中缀运算符 operator 是 +，那么表达式的计算公式即 args[0] operator args[1]
#define PRIM_NUM_INFIX(name, operator, type)           \
    static bool name(VM *vm, Value *args) {            \
        if (!validateNum(vm, args[1])) {               \
            return false;                              \
        }                                              \
        uint32_t leftOperand = VALUE_TO_NUM(args[0]);  \
        uint32_t rightOperand = VALUE_TO_NUM(args[1]); \
        RET_##type(leftOperand operator rightOperand); \
    }

PRIM_NUM_INFIX(primNumPlus, +, NUM);
PRIM_NUM_INFIX(primNumMinus, -, NUM);
PRIM_NUM_INFIX(primNumMul, *, NUM);
PRIM_NUM_INFIX(primNumDiv, /, NUM);
PRIM_NUM_INFIX(primNumGt, >, BOOL);
PRIM_NUM_INFIX(primNumGe, >=, BOOL);
PRIM_NUM_INFIX(primNumLt, <, BOOL);
PRIM_NUM_INFIX(primNumLe, <=, BOOL);
#undef PRIM_NUM_INFIX

// 定义 Num 相关位操作的宏，原理和上面一样
#define PRIM_NUM_BIT(name, operator)                   \
    static bool name(VM *vm, Value *args) {            \
        if (!validateNum(vm, args[1])) {               \
            return false;                              \
        }                                              \
        uint32_t leftOperand = VALUE_TO_NUM(args[0]);  \
        uint32_t rightOperand = VALUE_TO_NUM(args[1]); \
        RET_NUM(leftOperand operator rightOperand);    \
    }

PRIM_NUM_BIT(primNumBitAnd, &);
PRIM_NUM_BIT(primNumBitOr, |);
PRIM_NUM_BIT(primNumBitShiftRight, >>);
PRIM_NUM_BIT(primNumBitShiftLeft, <<);
#undef PRIM_NUM_BIT

// 使用数学库函数的宏
#define PRIM_NUM_MATH_FN(name, mathFn)             \
    static bool name(VM *vm UNUSED, Value *args) { \
        RET_NUM(mathFn(VALUE_TO_NUM(args[0])));    \
    }

PRIM_NUM_MATH_FN(primNumAbs, fabs);
PRIM_NUM_MATH_FN(primNumAcos, acos);
PRIM_NUM_MATH_FN(primNumAsin, asin);
PRIM_NUM_MATH_FN(primNumAtan, atan);
PRIM_NUM_MATH_FN(primNumCeil, ceil);
PRIM_NUM_MATH_FN(primNumCos, cos);
PRIM_NUM_MATH_FN(primNumFloor, floor);
PRIM_NUM_MATH_FN(primNumNegate, -);
PRIM_NUM_MATH_FN(primNumSin, sin);
PRIM_NUM_MATH_FN(primNumSqrt, sqrt);
PRIM_NUM_MATH_FN(primNumTan, tan);
#undef PRIM_NUM_MATH_FN

// 数字取模
// 该方法是脚本中调用 num1%num2 所执行的原生方法，该方法为实例方法
static bool primNumMod(VM *vm UNUSED, Value *args) {
    if (!validateNum(vm, args[1])) {
        return false;
    }
    RET_NUM(fmod(VALUE_TO_NUM(args[0]), VALUE_TO_NUM(args[1])));
}

// 数字取反
// 该方法是脚本中调用 ~num 所执行的原生方法，该方法为实例方法
static bool primNumBitNot(VM *vm UNUSED, Value *args) {
    RET_NUM(~(uint32_t)VALUE_TO_NUM(args[0]));
}

// 数字获取范围
// 该方法是脚本中调用[num1..num2] 所执行的原生方法，该方法为实例方法
static bool primNumRange(VM *vm UNUSED, Value *args) {
    if (!validateNum(vm, args[1])) {
        return false;
    }

    double from = VALUE_TO_NUM(args[0]);
    double to = VALUE_TO_NUM(args[1]);
    RET_OBJ(newObjRange(vm, from, to));
}

// 取数字的整数部分
// 该方法是脚本中调用 num.truncate 所执行的原生方法，该方法为实例方法
static bool primNumTruncate(VM *vm UNUSED, Value *args) {
    double integer;
    modf(VALUE_TO_NUM(args[0]), &integer);
    RET_NUM(integer);
}

// 返回小数部分
// 该方法是脚本中调用 num.fraction 所执行的原生方法，该方法为实例方法
static bool primNumFraction(VM *vm UNUSED, Value *args) {
    double dummyInteger;
    RET_NUM(modf(VALUE_TO_NUM(args[0]), &dummyInteger));
}

// 判断数字是否无穷大，不区分正负无穷大
// 该方法是脚本中调用 num.isInfinity 所执行的原生方法，该方法为实例方法
static bool primNumIsInfinity(VM *vm UNUSED, Value *args) {
    RET_BOOL(isinf(VALUE_TO_NUM(args[0])));
}

// 判断是否为整数
// 该方法是脚本中调用 num.isInteger 所执行的原生方法，该方法为实例方法
static bool primNumIsInteger(VM *vm UNUSED, Value *args) {
    double num = VALUE_TO_NUM(args[0]);
    // 如果是 NaN (不是一个数字)或无限大的数字就返回 false
    if (isnan(num) || isinf(num)) {
        RET_FALSE;
    }
    RET_BOOL(trunc(num) == num);
}

// 判断数字是否为 NaN
// 该方法是脚本中调用 num.isNan 所执行的原生方法，该方法为实例方法
static bool primNumIsNan(VM *vm UNUSED, Value *args) {
    RET_BOOL(isnan(VALUE_TO_NUM(args[0])));
}

// 数字转换为字符串
// 该方法是脚本中调用 num.toString 所执行的原生方法，该方法为实例方法
static bool primNumToString(VM *vm UNUSED, Value *args) {
    RET_OBJ(num2str(vm, VALUE_TO_NUM(args[0])));
}

// 判断两个数字是否相等
// 该方法是脚本中调用 num1 == num2 所执行的原生方法，该方法为实例方法
static bool primNumEqual(VM *vm UNUSED, Value *args) {
    if (!validateNum(vm, args[1])) {
        RET_FALSE;
    }

    RET_BOOL(VALUE_TO_NUM(args[0]) == VALUE_TO_NUM(args[1]));
}

// 判断两个数字是否不等
// 该方法是脚本中调用 num1 != num2 所执行的原生方法，该方法为实例方法
static bool primNumNotEqual(VM *vm UNUSED, Value *args) {
    if (!validateNum(vm, args[1])) {
        RET_TRUE;
    }
    RET_BOOL(VALUE_TO_NUM(args[0]) != VALUE_TO_NUM(args[1]));
}

/**
 * String 类的原生方法
**/

// 基于码点 args[1] 创建字符串
// 该方法是脚本中调用 ObjString.fromCodePoint(args[1]) 所执行的原生方法，该方法为类方法
static bool primStringFromCodePoint(VM *vm, Value *args) {
    if (!validateInt(vm, args[1])) {
        return false;
    }

    int codePoint = (int)VALUE_TO_NUM(args[1]);
    if (codePoint < 0) {
        SET_ERROR_FALSE(vm, "code point can`t be negetive!");
    }

    if (codePoint > 0x10ffff) {
        SET_ERROR_FALSE(vm, "code point must be between 0 and 0x10ffff!");
    }

    RET_VALUE(makeStringFromCodePoint(vm, codePoint));
}

// 字符串相加
// 该方法是脚本中调用 oargs[0] +args[1] 所执行的原生方法，该方法为实例方法
static bool primStringPlus(VM *vm, Value *args) {
    if (!validateString(vm, args[1])) {
        return false;
    }

    ObjString *left = VALUE_TO_OBJSTR(args[0]);
    ObjString *right = VALUE_TO_OBJSTR(args[1]);

    // 结果字符串 result 长度为两个字符串长度之和
    uint32_t totalLength = strlen(left->value.start) + strlen(right->value.start);

    // +1 是为了结尾的 '\0'
    // 为结果字符串 result 申请内存空间
    ObjString *result = ALLOCATE_EXTRA(vm, ObjString, totalLength + 1);
    if (result == NULL) {
        MEM_ERROR("allocate memory failed in runtime!");
    }

    initObjHeader(vm, &result->objHeader, OT_STRING, vm->stringClass);
    // 分别将 left->value 和 right->value 拷贝到 result->value 中
    memcpy(result->value.start, left->value.start, strlen(left->value.start));
    memcpy(result->value.start + strlen(left->value.start), right->value.start, strlen(right->value.start));
    result->value.start[totalLength] = '\0';
    result->value.length = totalLength;

    // 根据字符串对象中的值 result->value 设置对应的哈希值给 result->hashCode
    hashObjString(result);

    RET_OBJ(result);
}

// 索引字符串
// 通过数字或 objRange 对象作为索引，获取字符串中的部分字符串
// 该方法是脚本中调用 objString[args[1]] 所执行的原生方法，其中 nargs[1]为数字或者 objRange 对象，该方法为实例方法
static bool primStringSubscript(VM *vm, Value *args) {
    ObjString *objString = VALUE_TO_OBJSTR(args[0]);

    // 索引可以是数字或 objRange 对象
    // 1. 如果索引是数字，就直接索引 1 个字符
    if (VALUE_IS_NUM(args[1])) {
        // 先判断该数字是否是在 [0, objString->value.length) 区间
        uint32_t index = validateIndex(vm, args[1], objString->value.length);
        if (index == UINT32_MAX) {
            return false;
        }
        // 若数字合法，则调用 stringCodePointAt 为该索引处的字符生成字符串对象并返回
        RET_VALUE(stringCodePointAt(vm, objString, index));
    }

    // 2. 如果索引不是数字，必定为 objRange 对象，否则报错
    if (!VALUE_IS_OBJRANGE(args[1])) {
        SET_ERROR_FALSE(vm, "subscript should be integer or range!");
    }

    // direction是索引的方向，1 表示正方向，即索引值递增，-1表示反方向，即索引值递减
    // from 若比 to大，即索引值递减，则为反方向，direction 为 1
    int direction;
    uint32_t count = objString->value.length;

    // 返回的 startIndex 是 objRange.from 在 objString.value.start 中的下标
    // calculateRange 主要是判断 objRange.from 和 objRange.to 是否在 [0, objString->value.length) 区间内，即索引范围是否合法
    uint32_t startIndex = calculateRange(vm, VALUE_TO_OBJRANGE(args[1]), &count, &direction);
    if (startIndex == UINT32_MAX) {
        return false;
    }
    // 从字符串 sourceStr 中获取起始为 startIndex，方向为 direction 的 count 个字符，创建字符串并返回
    RET_OBJ(newObjStringFromSub(vm, objString, startIndex, count, direction));
}

// 获取字符串中指定索引的字符对应的字节
// 该方法是脚本中调用 objString.byteAt_(args[1]) 所执行的原生方法，其中 args[1]为索引，该方法为实例方法
static bool primStringByteAt(VM *vm UNUSED, Value *args) {
    ObjString *objString = VALUE_TO_OBJSTR(args[0]);
    // 先判断该索引 args[1] 是否是在 [0, objString->value.length) 区间
    uint32_t index = validateIndex(vm, args[1], objString->value.length);
    if (index == UINT32_MAX) {
        return false;
    }
    // 如果索引合法，则返回对应字符的数字形式
    RET_NUM((uint8_t)objString->value.start[index]);
}

// 获取字符串对应的字节数
// 该方法是脚本中调用 objString.byteCount_ 所执行的原生方法，该方法为实例方法
static bool primStringByteCount(VM *vm UNUSED, Value *args) {
    ObjString *objString = VALUE_TO_OBJSTR(args[0]);
    RET_NUM(objString->value.length);
}

// 获取字符串中指定索引的字符对应的码点
// 该方法是脚本中调用 objString.codePointAt_(args[1]) 所执行的原生方法，该方法为实例方法
static bool primStringCodePointAt(VM *vm UNUSED, Value *args) {
    ObjString *objString = VALUE_TO_OBJSTR(args[0]);
    // 先判断该索引 args[1] 是否是在 [0, objString->value.length) 区间
    uint32_t index = validateIndex(vm, args[1], objString->value.length);
    if (index == UINT32_MAX) {
        return false;
    }

    const uint8_t *bytes = (uint8_t *)objString->value.start;
    if ((bytes[index] & 0xc0) == 0x80) {
        // 如果 index 指向的并不是 UTF-8 编码的最高字节
        // 而是后面的低字节,返回 -1 提示用户
        RET_NUM(-1);
    }

    // 调用 decodeUtf8 解码对应字符并返回
    RET_NUM(decodeUtf8((uint8_t *)objString->value.start + index, objString->value.length - index));
}

// 判断字符串 args[0] 中是否包含子字符串 args[1]
// 该方法是脚本中调用 objString.contains(args[1]) 所执行的原生方法，该方法为实例方法
static bool primStringContains(VM *vm UNUSED, Value *args) {
    // 先校验参数 args[1] 是否为字符串
    if (!validateString(vm, args[1])) {
        return false;
    }

    ObjString *objString = VALUE_TO_OBJSTR(args[0]);
    ObjString *pattern = VALUE_TO_OBJSTR(args[1]);

    // 调用 findString 来判断字符串 args[0] 中是否包含子字符串 args[1]
    RET_BOOL(findString(objString, pattern) != -1);
}

// 检索字符串 args[0] 中子串 args[1] 的起始下标
// 该方法是脚本中调用 objString.indexOf(args[1]) 所执行的原生方法，该方法为实例方法
static bool primStringIndexOf(VM *vm UNUSED, Value *args) {
    // 先校验参数 args[1] 是否为字符串
    if (!validateString(vm, args[1])) {
        return false;
    }

    ObjString *objString = VALUE_TO_OBJSTR(args[0]);
    ObjString *pattern = VALUE_TO_OBJSTR(args[1]);

    // 若 pattern 比源串 objString 还长，源串 objString 必然不包括 pattern
    if (pattern->value.length > objString->value.length) {
        RET_FALSE;
    }

    // 否则调用 findString 来检索字符串 args[0] 中子串 args[1] 的起始下标
    int index = findString(objString, pattern);
    RET_NUM(index);
}

// 判断字符串 args[0] 是否以字符串 args[1] 为开始
// 该方法是脚本中调用 objString.startsWith(args[1]) 所执行的原生方法，该方法为实例方法
static bool primStringStartsWith(VM *vm UNUSED, Value *args) {
    // 先校验参数 args[1] 是否为字符串
    if (!validateString(vm, args[1])) {
        return false;
    }

    ObjString *objString = VALUE_TO_OBJSTR(args[0]);
    ObjString *pattern = VALUE_TO_OBJSTR(args[1]);

    // 若 pattern 比源串 objString 还长，源串 objString 必然不包括 pattern
    if (pattern->value.length > objString->value.length) {
        RET_FALSE;
    }

    // 否则调用 memcmp 函数比较相同位置的字符串是否相同
    RET_BOOL(memcmp(objString->value.start, pattern->value.start, pattern->value.length) == 0);
}

// 判断字符串 args[0] 是否以字符串 args[1] 为结束
// 该方法是脚本中调用 objString.endsWith(args[1]) 所执行的原生方法，该方法为实例方法
static bool primStringEndsWith(VM *vm UNUSED, Value *args) {
    // 先校验参数 args[1] 是否为字符串
    if (!validateString(vm, args[1])) {
        return false;
    }

    ObjString *objString = VALUE_TO_OBJSTR(args[0]);
    ObjString *pattern = VALUE_TO_OBJSTR(args[1]);

    // 若 pattern 比源串 objString 还长，源串 objString 必然不包括 pattern
    if (pattern->value.length > objString->value.length) {
        RET_FALSE;
    }

    // 否则调用 memcmp 函数比较相同位置的字符串是否相同
    char *cmpIdx = objString->value.start + objString->value.length - pattern->value.length;
    RET_BOOL(memcmp(cmpIdx, pattern->value.start, pattern->value.length) == 0);
}

// 字符串输出自己
// 该方法是脚本中调用 objString.toString() 所执行的原生方法，该方法为实例方法
static bool primStringToString(VM *vm UNUSED, Value *args) {
    RET_VALUE(args[0]);
}

/**
 * List 类的原生方法
**/

// 创建 list 实例
// 该方法是脚本中调用 ObjList.new() 所执行的原生方法，该方法为类方法
static bool primListNew(VM *vm, Value *args UNUSED) {
    // 返回列表自身
    RET_OBJ(newObjList(vm, 0));
}

// 索引 list 中的元素（索引可以是数字或者 objRange 实例）
// 该方法是脚本中调用 objList[args[1]] 所执行的原生方法，该方法为实例方法
static bool primListSubscript(VM *vm, Value *args) {
    ObjList *objList = VALUE_TO_OBJLIST(args[0]);

    // 索引可以是数字或 objRange 对象
    // 1. 如果索引是数字，就直接索引 1 个字符
    if (VALUE_IS_NUM(args[1])) {
        // 先判断该数字是否是在 [0, objString->value.length) 区间
        uint32_t index = validateIndex(vm, args[1], objList->elements.count);
        if (index == UINT32_MAX) {
            return false;
        }
        // 若数字合法，则获取该元素并返回
        RET_VALUE(objList->elements.datas[index]);
    }

    // 2. 如果索引不是数字，必定为 objRange 对象，否则报错
    if (!VALUE_IS_OBJRANGE(args[1])) {
        SET_ERROR_FALSE(vm, "subscript should be integer or range!");
    }

    // direction是索引的方向，1 表示正方向，即索引值递增，-1表示反方向，即索引值递减
    // from 若比 to大，即索引值递减，则为反方向，direction 为 1
    int direction;
    uint32_t count = objList->elements.count;

    // 返回的 startIndex 是 objRange.from 在 objString.value.start 中的下标
    // calculateRange 主要是判断 objRange.from 和 objRange.to 是否在 [0, objString->value.length) 区间内，即索引范围是否合法
    uint32_t startIndex = calculateRange(vm, VALUE_TO_OBJRANGE(args[1]), &count, &direction);

    // 新建一个 list 存储该 range 在原来 list 中索引的元素
    ObjList *result = newObjList(vm, count);
    uint32_t idx = 0;
    while (idx < count) {
        // direction为 -1 表示从后往前倒序赋值
        // 如 var l = [a,b,c,d,e,f,g]; l[5..3]表示[f,e,d]
        result->elements.datas[idx] = objList->elements.datas[startIndex + idx * direction];
        idx++;
    }
    RET_OBJ(result);
}

// 对 list 中某个索引的元素赋值（索引只能是数字）
// 该方法是脚本中调用 objList[args[1]] = args[2] 所执行的原生方法，该方法为实例方法
static bool primListSubscriptSetter(VM *vm UNUSED, Value *args) {
    // 获取对象
    ObjList *objList = VALUE_TO_OBJLIST(args[0]);

    // 获取索引
    uint32_t index = validateIndex(vm, args[1], objList->elements.count);
    if (index == UINT32_MAX) {
        return false;
    }

    // 直接赋值
    objList->elements.datas[index] = args[2];

    // 把要赋的值 args[2] 做为返回值
    RET_VALUE(args[2]);
}

// 向 list 后面追加元素
// 该方法是脚本中调用 objList.add(args[1]) 所执行的原生方法，该方法为实例方法
static bool primListAdd(VM *vm, Value *args) {
    ObjList *objList = VALUE_TO_OBJLIST(args[0]);
    ValueBufferAdd(vm, &objList->elements, args[1]);
    // 将要追加的元素 args[1] 做为返回值
    RET_VALUE(args[1]);
}

// 向 list 后面追加元素
// 该方法是脚本中调用 objList.addCore_(args[1]) 所执行的原生方法，该方法为实例方法
// 该方法主要用于内部使用，主要是为了支持字面量形式创建的 list 而非 List.new() 方式，例如 var l = [1, 4, 7];
static bool primListAddCore(VM *vm, Value *args) {
    ObjList *objList = VALUE_TO_OBJLIST(args[0]);
    ValueBufferAdd(vm, &objList->elements, args[1]);
    // 返回列表自身
    RET_VALUE(args[0]);
}

// 向 list 中某个位置插入元素
// 该方法是脚本中调用 objList.insert(args[1], args[2]) 所执行的原生方法，args[1] 为索引，args[2] 为元素，该方法为实例方法
static bool primListInsert(VM *vm, Value *args) {
    ObjList *objList = VALUE_TO_OBJLIST(args[0]);
    // +1 确保可以在最后插入
    uint32_t index = validateIndex(vm, args[1], objList->elements.count + 1);
    if (index == UINT32_MAX) {
        return false;
    }
    insertElement(vm, objList, index, args[2]);
    // 元素 args[2] 做为返回值
    RET_VALUE(args[2]);
}

// 删除 list 中某个位置的元素
// 该方法是脚本中调用 objList.removeAt(args[1]) 所执行的原生方法，该方法为实例方法
static bool primListRemoveAt(VM *vm, Value *args) {
    //获取实例对象
    ObjList *objList = VALUE_TO_OBJLIST(args[0]);

    uint32_t index = validateIndex(vm, args[1], objList->elements.count);
    if (index == UINT32_MAX) {
        return false;
    }
    // 被删除的元素做为返回值
    RET_VALUE(removeElement(vm, objList, index));
}

// 清空 list 中所有元素
// 该方法是脚本中调用 objList.clear() 所执行的原生方法，该方法为实例方法
static bool primListClear(VM *vm, Value *args) {
    ObjList *objList = VALUE_TO_OBJLIST(args[0]);
    ValueBufferClear(vm, &objList->elements);
    RET_NULL;
}

// 返回 list 的元素个数
// 该方法是脚本中调用 objList.count 所执行的原生方法，该方法为实例方法
static bool primListCount(VM *vm UNUSED, Value *args) {
    ObjList *objList = VALUE_TO_OBJLIST(args[0]);
    RET_NUM(objList->elements.count);
}

/**
 * Map 类的原生方法
**/

// 创建 map 实例
// 该方法是脚本中调用 ObjMap.new() 所执行的原生方法，该方法为类方法
static bool primMapNew(VM *vm, Value *args UNUSED) {
    RET_OBJ(newObjMap(vm));
}

// 获取 map[key] 对应的 value
// 该方法是脚本中调用 objMap[args[1]] 所执行的原生方法，该方法为类方法
static bool primMapSubscript(VM *vm, Value *args) {
    // 先校验 key 的合法性
    if (!validateKey(vm, args[1])) {
        return false;
    }

    // 获得 map 对象实例
    ObjMap *objMap = VALUE_TO_OBJMAP(args[0]);

    // 从 map 中查找 key 即 args[1] 对应的 value
    Value value = mapGet(objMap, args[1]);

    // 若没有相应的 key 则返回 NULL
    if (VALUE_IS_UNDEFINED(value)) {
        RET_NULL;
    }
    RET_VALUE(value);
}

// 对 map[key] 赋值
// 该方法是脚本中调用 objMap[args[1]] = args[2] 所执行的原生方法，其中 args[1] 为 key，args[2] 为要赋的值，该方法为实例方法
static bool primMapSubscriptSetter(VM *vm, Value *args) {
    // 先校验 key 的合法性
    if (!validateKey(vm, args[1])) {
        return false;
    }

    // 获得 map 对象实例
    ObjMap *objMap = VALUE_TO_OBJMAP(args[0]);

    // 在 map 中将 key 和 value 关联，即 map[key] = value
    mapSet(vm, objMap, args[1], args[2]);
    RET_VALUE(args[2]);
}

// 在 map 中添加 key-value 对并返回 map 自身
// 该方法是脚本中调用 objMap.addCore_(args[1], args[2]) 所执行的原生方法，该方法为实例方法
// 该方法主要用于内部使用，主要是为了支持字面量形式创建的 map 而非 Map.new() 方式，例如 var map = {a: 1};
static bool primMapAddCore(VM *vm, Value *args) {
    // 先校验 key 的合法性
    if (!validateKey(vm, args[1])) {
        return false;
    }

    //获得 map 对象实例
    ObjMap *objMap = VALUE_TO_OBJMAP(args[0]);

    // 在 map 中将 key 和 value 关联，即 map[key] = value
    mapSet(vm, objMap, args[1], args[2]);
    // 返回 map 对象自身
    RET_VALUE(args[0]);
}

// 删除 map 中对应 key 的 entry（即 key-value 对）
// 该方法是脚本中调用 objMap.remove(args[1]) 所执行的原生方法，该方法为实例方法
static bool primMapRemove(VM *vm, Value *args) {
    // 先校验 key 的合法性
    if (!validateKey(vm, args[1])) {
        return false;
    }

    RET_VALUE(removeKey(vm, VALUE_TO_OBJMAP(args[0]), args[1]));
}

// 清空 map
// 该方法是脚本中调用 objMap.clear() 所执行的原生方法，该方法为实例方法
static bool primMapClear(VM *vm, Value *args) {
    clearMap(vm, VALUE_TO_OBJMAP(args[0]));
    RET_NULL;
}

// 判断 map 即 args[0] 是否包含 key 即 args[1]
// 该方法是脚本中调用 objMap.containsKey(args[1]) 所执行的原生方法，该方法为实例方法
static bool primMapContainsKey(VM *vm, Value *args) {
    // 先校验 key 的合法性
    if (!validateKey(vm, args[1])) {
        return false;
    }

    // 从 map 中获取该 key 对应的 value，如果 value 存在则 key 存在，否则不存在
    RET_BOOL(!VALUE_IS_UNDEFINED(mapGet(VALUE_TO_OBJMAP(args[0]), args[1])));
}

// 获取 map 中 entry 个数，即 key-value 的对数
// 该方法是脚本中调用 objMap.count 所执行的原生方法，该方法为实例方法
static bool primMapCount(VM *vm UNUSED, Value *args) {
    ObjMap *objMap = VALUE_TO_OBJMAP(args[0]);
    RET_NUM(objMap->count);
}

/**
 * range 类的原生方法
**/

// 返回 range 实例对象的 from 属性的值
// 该方法是脚本中调用 objRange.from 所执行的原生方法，该方法为实例方法
static bool primRangeFrom(VM *vm UNUSED, Value *args) {
    ObjRange *objRange = VALUE_TO_OBJRANGE(args[0]);
    RET_NUM(objRange->from);
}

// 返回 range 实例对象的 to 属性的值
// 该方法是脚本中调用 objRange.to 所执行的原生方法，该方法为实例方法
static bool primRangeTo(VM *vm UNUSED, Value *args) {
    ObjRange *objRange = VALUE_TO_OBJRANGE(args[0]);
    RET_NUM(objRange->to);
}

// 返回 range 实例对象的 from 属性和 to 属性的中的较小值
// 该方法是脚本中调用 objRange.min 所执行的原生方法，该方法为实例方法
static bool primRangeMin(VM *vm UNUSED, Value *args) {
    ObjRange *objRange = VALUE_TO_OBJRANGE(args[0]);
    RET_NUM(fmin(objRange->from, objRange->to));
}

// 返回 range 实例对象的 from 属性和 to 属性的中的较大值
// 该方法是脚本中调用 objRange.max 所执行的原生方法，该方法为实例方法
static bool primRangeMax(VM *vm UNUSED, Value *args) {
    ObjRange *objRange = VALUE_TO_OBJRANGE(args[0]);
    RET_NUM(fmax(objRange->from, objRange->to));
}

/**
 * System 类的原生方法
**/

// 返回以秒为单位的系统时钟
// 该方法是脚本中调用 System.clock 所执行的原生方法，该方法为类方法
static bool primSystemClock(VM *vm UNUSED, Value *args UNUSED) {
    RET_NUM((double)time(NULL));
}

// 启动 gc
// 该方法是脚本中调用 System.gc() 所执行的原生方法，该方法为类方法
static bool primSystemGC(VM *vm, Value *args) {
    // startGC(vm);
    RET_NULL;
}

// 导入并编译模块（即将模块挂载到 vm->allModules）
// 该方法是脚本中调用 System.importModule(args[1]) 所执行的原生方法，该方法为类方法
static bool primSystemImportModule(VM *vm, Value *args) {
    if (!validateString(vm, args[1])) { //模块名为字符串
        return false;
    }

    // 导入模块并编译（即将模块挂载到 vm->allModules）
    Value result = importModule(vm, args[1]);

    // 若模块已经导入（模块导入时 importModule 返回 NULL），则返回 NULL
    if (VALUE_IS_NULL(result)) {
        RET_NULL;
    }

    // 若模块编译过程中出了问题，则返回 false，通知虚拟机切换线程
    if (!VALUE_IS_NULL(vm->curThread->errorObj)) {
        return false;
    }

    // 调用本函数时，已经在主调方的运行时栈中压入了 System 和 moduleName 两个 slot
    // 而只需要一个 slot 来存储返回值，所以回收一个 slot
    vm->curThread->esp--;

    ObjThread *nextThread = VALUE_TO_OBJTHREAD(result);
    nextThread->caller = vm->curThread;
    vm->curThread = nextThread;
    // 返回 false，虚拟机会切换到此新加载模块的线程 nextThread
    return false;
}

// 获取模块 args[1] 中的模块变量 args[2]
// 该方法是脚本中调用 System.getModuleVariable(args[1], args[2]) 所执行的原生方法，该方法为类方法
static bool primSystemGetModuleVariable(VM *vm, Value *args) {
    if (!validateString(vm, args[1])) {
        return false;
    }

    if (!validateString(vm, args[2])) {
        return false;
    }

    Value result = getModuleVariable(vm, args[1], args[2]);
    if (VALUE_IS_NULL(result)) {
        // 如果出错，则返回 false，通知虚拟机切换线程
        return false;
    }

    RET_VALUE(result);
}

// 输出字符串
// 该方法是脚本中调用 System.writeString_(args[1]) 所执行的原生方法，该方法为类方法
static bool primSystemWriteString(VM *vm UNUSED, Value *args) {
    ObjString *objString = VALUE_TO_OBJSTR(args[1]);

    ASSERT(objString->value.start[objString->value.length] == '\0', "string isn`t terminated!");

    printString(objString->value.start);
    RET_VALUE(args[1]);
}

/**
 * 至此，原生方法定义部分结束
**/

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

    /* Bool 类定义在 core.script.inc，将其挂载到 vm->boolClass，并绑定原生方法 */
    vm->boolClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "bool"));
    PRIM_METHOD_BIND(vm->boolClass, "toString", primBoolToString);
    PRIM_METHOD_BIND(vm->boolClass, "!", primBoolNot);

    /* Thread 类定义在 core.script.inc，将其挂载到 vm->threadClass，并绑定原生方法 */
    vm->threadClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Thread"));
    // 以下是 Thread 类方法
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "new(_)", primThreadNew);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "abort(_)", primThreadAbort);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "current", primThreadCurrent);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "suspend()", primThreadSuspend);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "yield(_)", primThreadYieldWithArg);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "yield()", primThreadYieldWithoutArg);
    // 以下是 Thread 实例方法
    PRIM_METHOD_BIND(vm->threadClass, "call()", primThreadCallWithoutArg);
    PRIM_METHOD_BIND(vm->threadClass, "call(_)", primThreadCallWithArg);
    PRIM_METHOD_BIND(vm->threadClass, "isDone", primThreadIsDone);

    /* Fn 类定义在 core.script.inc，将其挂载到 vm->fnClass，并绑定原生方法 */
    vm->fnClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Fn"));
    PRIM_METHOD_BIND(vm->fnClass->objHeader.class, "new(_)", primFnNew);

    // 绑定 call 的重载方法
    bindFnOverloadCall(vm, "call()");
    bindFnOverloadCall(vm, "call(_)");
    bindFnOverloadCall(vm, "call(_,_)");
    bindFnOverloadCall(vm, "call(_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_)");

    /* Null 类定义在 core.script.inc，将其挂载到 vm->nullClass，并绑定原生方法 */
    vm->nullClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Null"));
    PRIM_METHOD_BIND(vm->nullClass, "!", primNullNot);
    PRIM_METHOD_BIND(vm->nullClass, "toString", primNullToString);

    /* Num 类定义在 core.script.inc，将其挂载到 vm->numClass，并绑定原生方法 */
    vm->numClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Num"));
    // 以下是 Num 类方法
    PRIM_METHOD_BIND(vm->numClass->objHeader.class, "fromString(_)", primNumFromString);
    PRIM_METHOD_BIND(vm->numClass->objHeader.class, "pi", primNumPi);
    // 以下是 Num 实例方法
    PRIM_METHOD_BIND(vm->numClass, "+(_)", primNumPlus);
    PRIM_METHOD_BIND(vm->numClass, "-(_)", primNumMinus);
    PRIM_METHOD_BIND(vm->numClass, "*(_)", primNumMul);
    PRIM_METHOD_BIND(vm->numClass, "/(_)", primNumDiv);
    PRIM_METHOD_BIND(vm->numClass, ">(_)", primNumGt);
    PRIM_METHOD_BIND(vm->numClass, ">=(_)", primNumGe);
    PRIM_METHOD_BIND(vm->numClass, "<(_)", primNumLt);
    PRIM_METHOD_BIND(vm->numClass, "<=(_)", primNumLe);
    PRIM_METHOD_BIND(vm->numClass, "&(_)", primNumBitAnd);
    PRIM_METHOD_BIND(vm->numClass, "|(_)", primNumBitOr);
    PRIM_METHOD_BIND(vm->numClass, ">>(_)", primNumBitShiftRight);
    PRIM_METHOD_BIND(vm->numClass, "<<(_)", primNumBitShiftLeft);
    PRIM_METHOD_BIND(vm->numClass, "abs", primNumAbs);
    PRIM_METHOD_BIND(vm->numClass, "acos", primNumAcos);
    PRIM_METHOD_BIND(vm->numClass, "asin", primNumAsin);
    PRIM_METHOD_BIND(vm->numClass, "atan", primNumAtan);
    PRIM_METHOD_BIND(vm->numClass, "ceil", primNumCeil);
    PRIM_METHOD_BIND(vm->numClass, "cos", primNumCos);
    PRIM_METHOD_BIND(vm->numClass, "floor", primNumFloor);
    PRIM_METHOD_BIND(vm->numClass, "-", primNumNegate);
    PRIM_METHOD_BIND(vm->numClass, "sin", primNumSin);
    PRIM_METHOD_BIND(vm->numClass, "sqrt", primNumSqrt);
    PRIM_METHOD_BIND(vm->numClass, "tan", primNumTan);
    PRIM_METHOD_BIND(vm->numClass, "%(_)", primNumMod);
    PRIM_METHOD_BIND(vm->numClass, "~", primNumBitNot);
    PRIM_METHOD_BIND(vm->numClass, "..(_)", primNumRange);
    PRIM_METHOD_BIND(vm->numClass, "truncate", primNumTruncate);
    PRIM_METHOD_BIND(vm->numClass, "fraction", primNumFraction);
    PRIM_METHOD_BIND(vm->numClass, "isInfinity", primNumIsInfinity);
    PRIM_METHOD_BIND(vm->numClass, "isInteger", primNumIsInteger);
    PRIM_METHOD_BIND(vm->numClass, "isNan", primNumIsNan);
    PRIM_METHOD_BIND(vm->numClass, "toString", primNumToString);
    PRIM_METHOD_BIND(vm->numClass, "==(_)", primNumEqual);
    PRIM_METHOD_BIND(vm->numClass, "!=(_)", primNumNotEqual);

    /* String 类定义在 core.script.inc，将其挂载到 vm->stringClass，并绑定原生方法 */
    vm->stringClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "String"));
    // 以下是 String 类方法
    PRIM_METHOD_BIND(vm->stringClass->objHeader.class, "fromCodePoint(_)", primStringFromCodePoint);
    // 以下是 String 实例方法
    PRIM_METHOD_BIND(vm->stringClass, "+(_)", primStringPlus);
    PRIM_METHOD_BIND(vm->stringClass, "[_]", primStringSubscript);
    PRIM_METHOD_BIND(vm->stringClass, "byteAt_(_)", primStringByteAt);
    PRIM_METHOD_BIND(vm->stringClass, "byteCount_", primStringByteCount);
    PRIM_METHOD_BIND(vm->stringClass, "codePointAt_(_)", primStringCodePointAt);
    PRIM_METHOD_BIND(vm->stringClass, "contains(_)", primStringContains);
    PRIM_METHOD_BIND(vm->stringClass, "indexOf(_)", primStringIndexOf);
    PRIM_METHOD_BIND(vm->stringClass, "startsWith(_)", primStringStartsWith);
    PRIM_METHOD_BIND(vm->stringClass, "endsWith(_)", primStringEndsWith);
    PRIM_METHOD_BIND(vm->stringClass, "toString", primStringToString);
    PRIM_METHOD_BIND(vm->stringClass, "count", primStringByteCount);

    /* List 类定义在 core.script.inc，将其挂载到 vm->listClass，并绑定原生方法 */
    vm->listClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "List"));
    // 以下是 List 类方法
    PRIM_METHOD_BIND(vm->listClass->objHeader.class, "new()", primListNew);
    // 以下是 List 实例方法
    PRIM_METHOD_BIND(vm->listClass, "[_]", primListSubscript);
    PRIM_METHOD_BIND(vm->listClass, "[_]=(_)", primListSubscriptSetter);
    PRIM_METHOD_BIND(vm->listClass, "add(_)", primListAdd);
    PRIM_METHOD_BIND(vm->listClass, "addCore_(_)", primListAddCore);
    PRIM_METHOD_BIND(vm->listClass, "insert(_,_)", primListInsert);
    PRIM_METHOD_BIND(vm->listClass, "removeAt(_)", primListRemoveAt);
    PRIM_METHOD_BIND(vm->listClass, "clear()", primListClear);
    PRIM_METHOD_BIND(vm->listClass, "count", primListCount);

    /* Map 类定义在 core.script.inc，将其挂载到 vm->mapClass，并绑定原生方法 */
    vm->mapClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Map"));
    // 以下是 Map 类方法
    PRIM_METHOD_BIND(vm->mapClass->objHeader.class, "new()", primMapNew);
    // 以下是 Map 实例方法
    PRIM_METHOD_BIND(vm->mapClass, "[_]", primMapSubscript);
    PRIM_METHOD_BIND(vm->mapClass, "[_]=(_)", primMapSubscriptSetter);
    PRIM_METHOD_BIND(vm->mapClass, "addCore_(_,_)", primMapAddCore);
    PRIM_METHOD_BIND(vm->mapClass, "remove(_)", primMapRemove);
    PRIM_METHOD_BIND(vm->mapClass, "clear()", primMapClear);
    PRIM_METHOD_BIND(vm->mapClass, "containsKey(_)", primMapContainsKey);
    PRIM_METHOD_BIND(vm->mapClass, "count", primMapCount);

    /* range 类定义在 core.script.inc，将其挂载到 vm->rangeClass，并绑定原生方法 */
    vm->rangeClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Range"));
    // 以下是 range 实例方法
    PRIM_METHOD_BIND(vm->rangeClass, "from", primRangeFrom);
    PRIM_METHOD_BIND(vm->rangeClass, "to", primRangeTo);
    PRIM_METHOD_BIND(vm->rangeClass, "min", primRangeMin);
    PRIM_METHOD_BIND(vm->rangeClass, "max", primRangeMax);

    /* System 类定义在 core.script.inc，将其挂载到 vm->systemClass，并绑定原生方法 */
    Class *systemClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "System"));
    // 以下是 System 类方法
    PRIM_METHOD_BIND(systemClass->objHeader.class, "clock", primSystemClock);
    PRIM_METHOD_BIND(systemClass->objHeader.class, "gc()", primSystemGC);
    PRIM_METHOD_BIND(systemClass->objHeader.class, "importModule(_)", primSystemImportModule);
    PRIM_METHOD_BIND(systemClass->objHeader.class, "getModuleVariable(_,_)", primSystemGetModuleVariable);
    PRIM_METHOD_BIND(systemClass->objHeader.class, "writeString_(_)", primSystemWriteString);

    // 在核心自举过程中创建了很多 ObjString 对象，创建过程中需要调用 initObjHeader 初始化对象头，
    // 使其 class 指向 vm->stringClass，但那时的 vm->stringClass 尚未初始化，因此现在更正。

    // 例如 buildCore 函数中在 vm->stringClass 赋值之前执行的 loadModule 函数
    // loadModule 里调用的链路：loadModule -> compileModule -> compileProgram -> compileClassDefination -> newObjString -> initObjHeader
    // 其中 initObjHeader 函数中会将类头中的 class 成员指向 vm->stringClass，但那时的 vm->stringClass 尚未赋值
    ObjHeader *objHeader = vm->allObjects;
    while (objHeader != NULL) {
        if (objHeader->type == OT_STRING) {
            objHeader->class = vm->stringClass;
        }
        objHeader = objHeader->next;
    }
}
