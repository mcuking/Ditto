#include <string.h>
#include <sys/stat.h>
#include "core.h"
#include "vm.h"
#include "utils.h"
#include "class.h"
#include "compiler.h"

// 源码文件所在的根目录，其值是在 cli.c 文件中设置的
// 解释器运行时会获得源码文件所在路径并写入 rootDir
char *rootDir = NULL;

// 宏 CORE_MODULE 用来表示核心模块，值是 VT_NUL 的 Value 结构
#define CORE_MODULE VT_TO_VALUE(VT_NULL)

// 定义原生方法的返回值
// 将 value 存储在 arg[0] 中
// 返回 true 或 false，会使虚拟机在回收该参数时采用不同的回收策略
#define RET_VALUE(value) \
    do                   \
    {                    \
        arg[0] = value;  \
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
    do                                                                 \
    {                                                                  \
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
        if (globalIdx == -1)                                                              \
        {                                                                                 \
            globalIdx = addSymbol(vm, &vm->allMethodNames, methodName, length);           \
        }                                                                                 \
        Method method;                                                                    \
        method.type = MT_PRIMITIVE;                                                       \
        method.primFn = func;                                                             \
        bindMethod(vm, classPtr, (uint32_t)globalIdx, method);                            \
    }

// 读取源码文件的方法
// path 为源码路径
char *readFile(const char *path)
{
    //获取源码文件的句柄 file
    FILE *file = fopen(path, "r");
    if (file == NULL)
    {
        IO_ERROR("Couldn't open file \"%s\"", path);
    }

    struct stat fileStat;
    stat(path, &fileStat);
    size_t fileSize = fileStat.st_size;

    // 获取源码文件大小后，为源码字符串申请内存，多申请的1个字节是为了字符串结尾 \0
    char *fileContent = (char *)malloc(fileSize + 1);
    if (fileContent == NULL)
    {
        MEM_ERROR("Couldn't allocate memory for reading file \"%s\".\n", path);
    }

    size_t numRead = fread(fileContent, sizeof(char), fileSize, file);
    if (numRead < fileSize)
    {
        IO_ERROR("Couldn't read file \"%s\"", path);
    }
    // 字符串要以 \0 结尾
    fileContent[fileSize] = '\0';

    fclose(file);
    return fileContent;
}

// 执行模块，目前为空函数
VMResult executeModule(VM *vm, Value moduleName, const char *sourceCode)
{
    return VM_RESULT_ERROR;
}

// 编译核心模块
void buildCore(VM *vm)
{
    // 创建核心模块
    ObjModule *coreModule = newObjModule(vm, NULL);
    // 将核心模块 coreModule 收集到 vm->allModules 中
    // vm->allModules 的 key 为 CORE_MODULE， value 为 coreModule 的 Value 结构
    mapSet(vm, vm->allModules, CORE_MODULE, OBJ_TO_VALUE(coreModule));
}

// 在 table 中查找符号 symbol，找到后返回索引，否则返回 -1
int getIndexFromSymbolTable(SymbolTable *table, const char *symbol, uint32_t length)
{
}

// 向 table 中添加符号 symbol，并返回其索引
int addSymbol(VM *vm, SymbolTable *table, const char *symbol, uint32_t length)
{
}