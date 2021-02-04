#include "vm.h"
#include "compiler.h"
#include "core.h"
#include <stdlib.h>

// 初始化虚拟机
void initVM(VM *vm) {
    // 记录已经分配的内存总和
    vm->allocatedBytes = 0;
    // 当前词法分析器初始化为 NULL
    vm->curLexer = NULL;
    // 指向所有已分配对象链表的首节点，用于垃圾回收
    vm->allObjects = NULL;
    // 初始化模块集合
    vm->allModules = newObjMap(vm);
    // 初始化类的方法集合
    StringBufferInit(&vm->allMethodNames);
};

// 新建虚拟机
VM *newVM() {
    // 为虚拟机申请内存，返回一个指针指向虚拟机
    VM *vm = (VM *)malloc(sizeof(VM));

    // 申请内存失败
    if (vm == NULL) {
        MEM_ERROR("allocate VM failed!");
    }

    // 调用 initVM 对虚拟机进行初始化
    initVM(vm);
    // 编译核心模块
    buildCore(vm);
    return vm;
}

// 确保栈的容量及数据有效
// needSlots 表示栈最少具有的容量，如果当前栈容量 stackCapacity 大于需要的栈数量，则直接返回即可
void ensureStack(VM *vm, ObjThread *objThread, uint32_t needSlots) {
    if (objThread->stackCapacity > needSlots) {
        return;
    }

    // ceilToPowerOf2 找出大于等于 v 的最小的 2 次幂
    // 即容量的值需要是 2 的倍数
    uint32_t newStackCapacity = ceilToPowerOf2(needSlots);

    // 先将栈底记录下来，用来后面判断是否原地扩容
    // 背景知识：
    // 内存管理系统为了满足 realloc 这种扩容分配，会在所分配的空间上预留一部分空间以满足将来的原地扩容。
    // 如果扩容的增量大小还在预留的空间内，则原地扩容；
    // 如果扩容的大小大于预留的空间，则会重新找一块更大且连续的空间，同时将原内存空间的数据拷贝过去，并返回新分配的内存起始地址（不会继续利用之前的预留空间，否则会破坏虚拟地址的连续性）
    // 整个过程是内存管理系统自动完成的，不需要用户干涉
    // 但是这种开辟新空间的做法，会导致地址变化，相关指针就要更新为新地址，即如果该指针涉及原内存块，就需要调整该指针的值，确保指向正确的位置
    Value *oldStack = objThread->stack;

    // 扩大容量，申请内存，memManager 会返回新分配的内存起始地址，会被转成 Value 类型，起始地址被保存到 Value 结构体的 num 属性中
    objThread->stack = (Value *)memManager(vm, objThread->stack, objThread->stackCapacity * sizeof(Value), newStackCapacity * sizeof(Value));
    objThread->stackCapacity = newStackCapacity;

    // 申请内存后，将现在的栈底和之前保存的栈底做比较，如果相等则是原地扩容（即预留空间可以满足扩容的增量），否则就是重新开辟了一块内存空间
    long offset = objThread->stack - oldStack;

    // 如果是重新开辟一块内存空间，并将原来的数据拷贝到新空间中，
    // 就需要调整原指针的值，确保指向正确的位置
    if (offset != 0) {
        // 1.调整各个函数帧栈中的起始地址
        uint32_t idx = 0;
        while (idx < objThread->usedFrameNum) {
            objThread->frames[idx].stackStart += offset;
            idx++;
        }

        // 2.调整 “大栈” 的栈顶 esp
        objThread->esp += offset;

        // 3.调整自由变量 upvalue 中 localVarPtr (用于指向对应的自由变量 upvalue)
        ObjUpvalue *upvalue = objThread->openUpvalues;
        while (upvalue != NULL) {
            upvalue->localVarPtr += offset;
            upvalue = upvalue->next;
        }
    }
}

// 背景知识：
// 线程就是函数的容器，线程对象提供了一个 “大栈”，在线程中运行的多个函数会共享这个 “大栈”，各自使用其中一部分作为该函数闭包的运行时栈
// 线程就是任务调度器，会提供一个帧栈数组 frames，为每个函数闭包分配一个帧栈 frame（包括3个部分：1.运行时栈    2.待运行的指令流    3.当前运行的指令地址 ip）
// 其中运行时栈就是使用 Value 数组来模拟，关于 Value 请参考其结构定义部分

// 为线程 objThread 中运行的闭包函数 objClosure 准备帧栈 Frame，即闭包（函数或方法）的运行资源，包括如下：
// 1.运行时栈    2.待运行的指令流    3.当前运行的指令地址 ip
inline static void createFrame(VM *vm, ObjThread *objThread, ObjClosure *objClosure, int argNum) {
    // 如果当前使用的 frame 数量（算上这次使用的一个）大于 frame 的总容量，则将总容量扩大二倍
    if (objThread->usedFrameNum + 1 > objThread->frameCapacity) {
        uint32_t newCapacity = objThread->frameCapacity * 2;

        // 扩大容量，申请内存，memManager 会返回新分配的内存起始地址，会被转成 Frame 类型，起始地址被保存到 Frame 中的 stackStart 中的 num 属性中
        objThread->frames = (Frame *)memManager(vm, objThread->frames, objThread->frameCapacity * sizeof(Frame), newCapacity * sizeof(Frame));
        objThread->frameCapacity = newCapacity;
    }

    // 先计算目前 “大栈” 的大小：栈顶地址 - 栈底地址
    uint32_t stackSlots = (uint32_t)(objThread->esp - objThread->stack);
    // 再加上函数/方法执行时需要的最大的栈数，就是创建这次帧栈需要的栈的总大小
    uint32_t needSlots = stackSlots + objClosure->fn->maxStackSlotUsedNum;
    // 确保栈的容量及数据有效
    ensureStack(vm, objThread, needSlots);

    // 为线程 objThread 中运行的闭包函数 objClosure 准备帧栈 Frame
    // 第三个参数是被调用函数的帧栈在整个 “大栈” 中的起始地址
    // 减去参数个数，是为了函数闭包 objClosure 可以访问到栈中自己的参数（TODO: 暂未搞懂，后续回填）
    prepareFrame(objThread, objClosure, objThread->esp - argNum);
}

// 背景知识：
// 内层函数在引用外层函数中的局部变量，该局部变量对内层函数来说，就是自由变量 upvalue，其中又分为 open upvalue 和 closed upvalue
// open upvalue 是其指针 upvalue->localVarPtr 所指向的局部变量未被回收，仍在运行时栈中的 upvalue
// closed upvalue 是其指针 upvalue->localVarPtr 所指向的局部变量已经被回收，不在运行时栈的 upvalue
// 例如：当外层函数执行完闭，会将其在运行时栈的内存回收掉，其中就包括了局部变量，如果此局部变量被内层函数引用，且该内层函数又被外部使用时，
// 此时，就会将指针 upvalue->localVarPtr 指向的运行时栈中的局部变量的值，保存到 upvalue->closedUpvalue 变量中，
// 同时将指针 upvalue->localVarPtr 改为指向 upvalue->closedUpvalue，这个过程就是 open upvalue 转变为 closed upvalue 的过程，也就是关闭自由变量的操作
// 从而确保了就是被内层函数引用的局部变量在运行时栈中被回收了，内层函数仍可通过 upvalue->closedUpvalue 访问该局部变量的值。

// 注意：如果某个外层函数执行完，在运行时栈的内存被回收了，其作用域以及其内嵌更深的作用域的局部变量都应该被回收，而作用域越深的变量在运行时栈中的地址就会越大，
// （因为先调用外层函数，然后调用内层函数，所以外层函数的局部变量会先压入运行时栈，内层函数居后，而越后压入，地址也就越大）
// 所以只需要将指针 upvalue->localVarPtr 的值（被内层函数引用的局部变量的地址）大于某个值（例如 lastSlot）的所有 upvalue 都执行自由变量操作即可
// （upvalue 是以链表的形式保存，其中 objThread->openUpvalues 就是指向本线程中 “已经打开过的 upvalue” 的链表的首节点）

// 关闭自由变量 upvalue（注：满足其指针 upvalue->localVarPtr 大于 lastSlot 的自由变量）
static void closedUpvalue(ObjThread *objThread, Value *lastSlot) {
    ObjUpvalue *upvalue = objThread->openUpvalues;
    // 注意：在自由变量 upvalue 链表创建的时候，就保证了是按照 upvalue->localVarPtr 的值降序排序的，首节点的自由变量的 localVarPtr 最大
    while (upvalue != NULL && upvalue >= lastSlot) {
        // 将指针 upvalue->localVarPtr 指向的运行时栈中的局部变量的值，保存到 upvalue->closedUpvalue 变量中
        upvalue->closedUpvalue = *(upvalue->localVarPtr);
        // 将指针 upvalue->localVarPtr 改为指向 upvalue->closedUpvalue
        upvalue->localVarPtr = &(upvalue->closedUpvalue);
        // 获取自由变量 upvalue 链表中下一个自由变量 upvalue
        upvalue = upvalue->next;
    }
    objThread->openUpvalues = upvalue;
}

// 创建线程中已经打开过的 upvalue 的链表
// 指针 localVarPtr 就是指向运行时栈中的局部变量，按照 localVarPtr 的值倒序插入到该链表
static ObjUpvalue *createOpenUpvalue(VM *vm, ObjThread *objThread, Value *localVarPtr) {
    // 如果 objThread->openUpvalues 链表还未创建，则创建链表，首节点为基于参数 localVarPtr 的 upvalue
    if (objThread->openUpvalues == NULL) {
        objThread->openUpvalues = newObjUpvalue(vm, localVarPtr);
        return objThread->openUpvalues;
    }

    // 否则从前到后遍历链表，找到合适的位置插入新的 upvalue
    ObjUpvalue *preUpvalue = NULL;
    ObjUpvalue *upvalue = objThread->openUpvalues;

    // 因为  upvalue 链表已经默认按照 upvalue->localVarPtr 的值倒序排列，
    // 所以只要 upvalue->localVarPtr > localVarPtr，就继续向后遍历，直到不满足 upvalue->localVarPtr > localVarPtr 为止
    while (upvalue != NULL && upvalue->localVarPtr > localVarPtr) {
        preUpvalue = upvalue;
        upvalue = upvalue->next;
    }

    // 如果之前插入了该 upvalue，直接返回即可
    if (upvalue != NULL && upvalue->localVarPtr == localVarPtr) {
        return upvalue;
    }

    // 否则就创建一个新的 upvalue，并插入到 upvalue 链表中
    ObjUpvalue *newUpvalue = newObjUpvalue(vm, localVarPtr);

    if (preUpvalue == NULL) {
        // 如果 preUpvalue 仍为 NULL，说明上面的 while 循环没有执行，也就是说参数 localVarPtr 大于首节点 objThread->openUpvalues 的 localVarPtr
        // 所以需要将基于参数 localVarPtr 的 upvalue 设置为首节点
        objThread->openUpvalues = newUpvalue;
    } else {
        // 否则就在 preUpvalue 和 upvalue 之间插入基于参数 localVarPtr 的 upvalue
        preUpvalue->next = newUpvalue;
    }
    newUpvalue->next = upvalue;
    return newUpvalue;
}

// 校验基类合法性
// classNameValue 为子类类名，fieldNum 为子类的实例属性数量，superClassValue 为基类
static void validateSuperClass(VM *vm, Value classNameValue, uint32_t fieldNum, Value superClassValue) {
    // 首先确保 superClass 类型是 class
    if (!VALUE_IS_CLASS(superClassValue)) {
        ObjString *classNameString = VALUE_TO_OBJSTR(classNameValue);
        RUN_ERROR("class \"%s\" 's superClass is not a valid class!", classNameString->value.start);
    }

    Class *superClass = VALUE_TO_CLASS(superClassValue);

    // 基类不能是内建类
    if (superClass == vm->stringClass ||
        superClass == vm->mapClass ||
        superClass == vm->rangeClass ||
        superClass == vm->listClass ||
        superClass == vm->nullClass ||
        superClass == vm->boolClass ||
        superClass == vm->numClass ||
        superClass == vm->fnClass ||
        superClass == vm->threadClass) {
        RUN_ERROR("superClass mustn't be a builtin class!");
    }

    // 因为子类也会继承父类的实例属性，所以 子类本身的实例属性数量 + 基类的实例属性数量 不能超过 MAX_FIELD_NUM
    if (superClass->fieldNum + fieldNum > MAX_FIELD_NUM) {
        RUN_ERROR("number of field including super exceed %d!", MAX_FIELD_NUM);
    }
}

// 修正部分指令的操作数
static void patchOperand(Class *class, ObjFn *fn) {
    int ip = 0;
    OpCode opCode;

    while (true) {
        // 从头开始遍历字节码中的所有操作码
        opCode = fn->instrStream.datas[ip];
        // 指向操作数的第一个字节（操作数占用的字节数可能是 0个、1个、4个 等）
        ip++;

        switch (opCode) {
            case OPCODE_LOAD_FIELD:
            case OPCODE_STORE_FIELD:
            case OPCODE_LOAD_THIS_FIELD:
            case OPCODE_STORE_THIS_FIELD:
                // 子类的实例属性数量 = 子类本身的实例属性数量 + 基类本身的实例属性数量
                // 当编译子类时，基类可能还未编译，所以需要等到编译阶段完全结束后，
                // 在子类本身的实例属性数量的基础上在加上基类本身的实例属性数量
                // 该操作数表示子类的实例属性数量，只有一个字节
                fn->instrStream.datas[ip] += class->superClass->fieldNum;
                // 指向下一个操作码
                ip++;
                break;

            case OPCODE_SUPER0:
            case OPCODE_SUPER1:
            case OPCODE_SUPER2:
            case OPCODE_SUPER3:
            case OPCODE_SUPER4:
            case OPCODE_SUPER5:
            case OPCODE_SUPER6:
            case OPCODE_SUPER7:
            case OPCODE_SUPER8:
            case OPCODE_SUPER9:
            case OPCODE_SUPER10:
            case OPCODE_SUPER11:
            case OPCODE_SUPER12:
            case OPCODE_SUPER13:
            case OPCODE_SUPER14:
            case OPCODE_SUPER15:
            case OPCODE_SUPER16:
                // 操作码 OPCODE_SUPERx 用于调用基类的方法的
                // 其操作数有 4 个字节，其中前两个字节存储 基类方法在基类中的索引 methodIndex，即 super.method[methodIndex] 表示基类的方法
                // 后两个字节存储 基类在常量表中的索引 superClassIndex，即 constants[superClassIndex] 表示基类

                // 相关指令在 emitCallBySignature 函数中写入，当时考虑到基类还没有编译，所以暂时使用 VT_NULL 代替基类插入到常量表中，
                // 并将 VT_NULL 在常量表中的索引 作为 后两个字节的操作数，即基类在在常量表中的索引
                // 所以只需要将常量表中的 VT_NULL 替换回基类即可，无需修改表示索引的操作数

                // 先跳过操作数的前两个字节（用于存储 基类方法在基类中的索引 methodIndex）
                ip += 2;

                // 计算操作数的后两个字节所保存的值，即基类在常量表中的索引（采用大端字节序，即表示高位地址的值保存在低地址字节中）
                uint32_t superClassIndex = (fn->instrStream.datas[ip] << 8) | fn->instrStream.datas[ip + 1];
                // 将常量表中索引为 superClassIndex 的值替换成真正的基类
                fn->constants.datas[superClassIndex] = OBJ_TO_VALUE(class->superClass);

                // 再跳过操作数的后两个字节（用于存储 基类在常量表中的索引 superClassIndex），指向下一个操作码
                ip += 2;
                break;

            case OPCODE_CREATE_CLOSURE:
                // 操作码 OPCODE_CREATE_CLOSURE 的操作数为：前两个字节（用于存储待创建闭包的函数在常量表中索引）+ 不定字节数（用于存储形式为 {upvalue 是否是直接编译外层单元的局部变量，upvalue 在直接外层编译单元的索引} 的成对信息）
                // 具体细节请参考函数 endCompileUnit 中的注释

                // 计算操作数的前两个字节所保存的值，即待创建闭包的函数在常量表中索引（采用大端字节序，即表示高位地址的值保存在低地址字节中）
                uint32_t fnIndex = (fn->instrStream.datas[ip] << 8) | fn->instrStream.datas[ip + 1];
                // 从常量表中获取到该函数，递归调用 patchOperand 修正该函数的指令流（即字节码）中部分指令的操作数
                patchOperand(class, VALUE_TO_OBJFN(fn->constants.datas[fnIndex]));

                // 跳过 OPCODE_CREATE_CLOSURE 的操作数，指向下一个操作码
                // 通过 getBytesOfOperands 获取到某个操作码 OPCODE_CREATE_CLOSURE 的操作数占用的字节数
                ip += getBytesOfOperands(fn->instrStream.datas, fn->constants.datas, ip - 1);
                break;

            case OPCODE_END:
                // 遇到操作码 OPCODE_END，表示字节码已经结束，直接退出即可
                return;

            default:
                // 其他字节码不需要修正操作数，直接跳过指向下一个操作码即可
                // 通过 getBytesOfOperands 获取到某个操作码的操作数占用的字节数，直接跳过即可
                ip += getBytesOfOperands(fn->instrStream.datas, fn->constants.datas, ip - 1);
                break;
        }
    }
}

// 背景知识：
// 各类自己的 methods 数组和 vm->allMethodNames 长度保持一致，进而 vm->allMethodNames 中的方法名和各个类的 methods 数组对应方法体的索引值相等，
// 这样就可以通过相同的索引获取到方法体或者方法名
// 然而 vm->allMethodNames 只有一个，但会对应多个类，所以各个类的 methods 数组中的方法体数量必然会小于 vm->allMethodNames 中的方法名数量
// 为了保证一样长度，就需要将各个类的 methods 数组中无用的索引处用空占位填充

// 修正方法对应指令流中的操作数且绑定方法到指定类上
static void bindMethodAndPath(VM *vm, OpCode opCode, Class *class, uint32_t methodIndex, Value methodValue) {
    // 类的静态方法由【类的 meta 类】的 methods 数组来存储
    // 类的实例方法由【类本身】的 methods 数组来存储
    if (opCode == OPCODE_STATIC_METHOD) {
        // class->objHeader.class 为 class 的 meta 类
        class = class->objHeader.class;
    }

    // 创建要绑定的方法 method
    Method method;
    method.type = MT_SCRIPT;
    method.obj = VALUE_TO_OBJCLOSURE(methodValue);

    // 修正方法对应指令流中的操作数
    patchOperand(class, method.obj->fn);

    // 然后绑定方法到指定类上
    // 即将 method 插入到 class->methods.datas 数组中，索引为 methodIndex
    // class->methods.datas[methodIndex] = method
    bindMethod(vm, class, methodIndex, method);
}

// 背景知识：
// 线程中的 “大栈” 被在其中运行的所有函数闭包的运行时栈所占用，各自分一块作为自己的运行时栈，各分块不重合，但互相接壤
// “大栈” 的栈底是 ObjThread->stack，栈顶是 ObjThread->esp，而线程中各个闭包函数自己的运行时栈的栈底是 stackStart
// stackStart 记录了本运行时栈在 “大栈” 中的起始地址

// 执行指令
VMResult executeInstruction(VM *vm, register ObjThread *curThread) {

    vm->curThread = curThread;  // 当前正在执行的线程
    register Frame *curFrame;   // 当前帧栈 frame
    register Value *stackStart; // 当前帧栈 frame 对应的运行时栈的起始地址（栈底）
    register uint8_t *ip;       //  程序计数器，用于存储即将执行的下一条指令在指令流中的地址
    register ObjFn *fn;         // 当前运行的函数对应的指令流
    OpCode opCode;              // 代执行指令的操作码

// 定义操作运行时栈的宏
// esp 指针指向的是栈中下一个可写入数据的 slot，即栈顶的后一个 slot
#define PUSH(value) (*curThread->esp++ = value) // 压入栈顶
#define POP() (*(--curThread->esp))             // 弹出栈顶，并获得栈顶的数据
#define DROP() (curThread->esp--)               // 丢弃栈顶
#define PEEK() (*(curThread->esp - 1))          // 获得栈顶数据（不改变栈顶指针 esp）
#define PEEK2() (*(curThread->esp - 2))         // 获得次栈顶数据（不改变栈顶指针 esp）

// 定义读取指令流的宏
#define READ_BYTE() (*ip++)                                          // 从指令流中读取 1 个字节
#define READ_SHORT() (ip += 2, (uint16_t)(((ip[-2] << 8) | ip[-1]))) // 从指令流中读取 2 个字节，采用大端字节序（即数据的高字节在低地址，低字节在高地址）

// 帧栈 frame 就是函数的执行环境，每调用一个函数就要为其准备一个帧栈 frame
// 下面的宏 STORE_CUR_FRAME 和 LOAD_CUR_FRAME 就是用于指令单元（函数或方法）的帧栈 frame 的切换

// 备份当前帧栈 frame 对应的指令流进度指针 ip，以便后面重新回到该帧栈时，能够从之前指令流执行的位置继续执行
#define STORE_CUR_FRAME() curFrame->ip = ip

// 加载 curThread->frames 中最新的帧栈 frame
#define LOAD_CUR_FRAME()
    // frames 是数组，索引从 0 开始，所以 usedFrameNum - 1
    curFrame = &curThread->frames[curThread->usedFrameNum - 1];
    stackStart = curFrame->stackStart;
    ip = curFrame->ip;
    fn = curFrame->closure->fn;

// loopStart 标号作用：当执行完一条指令后，会直接 goto 到此标号，以减少 CPU 跳出各分支的消耗，以提升虚拟机速度
loopStart:
    // 读入指令流中的操作码
    opCode = READ_BYTE();
    switch (opCode) {
        case OPCODE_POP:
            //【弹出栈顶】
            DROP();
            goto loopStart;

        case OPCODE_PUSH_NULL:
            //【将 null 压入到运行时栈顶】
            PUSH(VT_TO_VALUE(VT_NULL));
            goto loopStart;

        case OPCODE_PUSH_TRUE:
            //【将 true 压入到运行时栈顶】
            PUSH(VT_TO_VALUE(VT_TRUE));
            goto loopStart;

        case OPCODE_PUSH_FALSE:
            //【将 false 压入到运行时栈顶】
            PUSH(VT_TO_VALUE(VT_FALSE));
            goto loopStart;

        case OPCODE_LOAD_CONSTANT:
            //【将常量的值压入到运行时栈顶】
            // 操作数为常量在常量表 constants 中的索引，占 2 个字节
            PUSH(fn->constants.datas[READ_SHORT()]);
            goto loopStart;

        case OPCODE_LOAD_THIS_FIELD:
            //【将类的实例属性的值加载到栈顶】
            // 操作数是该属性在 objInstance->fields 数组中的索引，占 1 个字节
            uint8_t fieldIndex = READ_BYTE();

            // 既然是加载实例属性，那么位于运行时栈底 stackStart[0] 应该是实例对象，否则报错
            ASSERT(VALUE_IS_OBJINSTANCE(stackStart[0]), "method receiver should be objInstance.");
            ObjInstance *objInstance = VALUE_TO_OBJINSTANCE(stackStart[0]);
            ASSERT(fieldIndex < objInstance->objHeader.class->fieldNum, "out of bounds field!");

            PUSH(objInstance->fields[fieldIndex]);
            goto loopStart;

        case OPCODE_LOAD_LOCAL_VAR:
            //【将局部变量在运行时栈的值压入到运行时栈顶】
            // 操作数为局部变量在运行时栈中的索引，占 1 个字节
            // 注意：cu->localVars 只是保存局部变量的名，局部变量的值是保存在运行时栈中的
            PUSH(stackStart[READ_BYTE()]);
            goto loopStart;

        case OPCODE_STORE_LOCAL_VAR:
            //【将运行时栈顶的值保存为局部变量的值，即将运行时栈顶的值写入到运行时栈中局部变量的相应位置】
            // 操作数为局部变量在运行时栈中的索引，占 1 个字节
            // 注意：cu->localVars 只是保存局部变量的名，局部变量的值是保存在运行时栈中的
            stackStart[READ_BYTE()] = PEEK();
            goto loopStart;

            {
                Class *class;   // 方法所属类
                int index;      // 方法在 class->methods 缓冲区中的索引
                Method *method; // 方法
                Value *args;    // 方法参数
                int argNum;     // 方法参数个数

                case OPCODE_CALL0:
                case OPCODE_CALL1:
                case OPCODE_CALL2:
                case OPCODE_CALL3:
                case OPCODE_CALL4:
                case OPCODE_CALL5:
                case OPCODE_CALL6:
                case OPCODE_CALL7:
                case OPCODE_CALL8:
                case OPCODE_CALL9:
                case OPCODE_CALL10:
                case OPCODE_CALL11:
                case OPCODE_CALL12:
                case OPCODE_CALL13:
                case OPCODE_CALL14:
                case OPCODE_CALL15:
                case OPCODE_CALL16:
                    // 方法参数个数
                    argNum = opCode - OPCODE_CALL0 + 1;

                    // 在调用方法之前，会提前将参数压入到运行时栈中，压入顺序是先压入前面的参数
                    // 因此 curThread->esp - argNum 指向的是第 0 个参数
                    args = curThread->esp - argNum;

                    // 分两种情况：
                    // 如果 OPCODE_CALLx 调用的是类的静态方法，则第一个参数 args[0] 是类，通过 getClassOfObj 函数获取的就是该类的 meta 类
                    // 如果 OPCODE_CALLx 调用的是类的静态方法，则第一个参数 args[0] 是实例对象，通过 getClassOfObj 函数获取的就是该实例对象所属的类
                    class = getClassOfObj(vm, args[0]);

                    // 操作数是方法在 class->methods 缓冲区中的索引，占 2 个字节
                    index = READ_SHORT();

                    // 从 class->methods 缓冲区取出方法
                    method = &class->methods.datas[index];

                    goto invokeMethod;

                case OPCODE_SUPER0:
                case OPCODE_SUPER1:
                case OPCODE_SUPER2:
                case OPCODE_SUPER3:
                case OPCODE_SUPER4:
                case OPCODE_SUPER5:
                case OPCODE_SUPER6:
                case OPCODE_SUPER7:
                case OPCODE_SUPER8:
                case OPCODE_SUPER9:
                case OPCODE_SUPER10:
                case OPCODE_SUPER11:
                case OPCODE_SUPER12:
                case OPCODE_SUPER13:
                case OPCODE_SUPER14:
                case OPCODE_SUPER15:
                case OPCODE_SUPER16:
                    // 方法参数个数
                    argNum = opCode - OPCODE_SUPER0 + 1;

                    // 在调用方法之前，会提前将参数压入到运行时栈中，压入顺序是先压入前面的参数
                    // 因此 curThread->esp - argNum 指向的是第 0 个参数
                    args = curThread->esp - argNum;

                    // 背景知识：
                    // OPCODE_SUPERx 的操作数有两个：
                    // 第 1 个是方法在基类 superClass 中 methods 的索引，即 superClass.methods[methodIndex]，占 2 个字节
                    // 第 2 个是基类 superClass 在常量表 constants 中的索引，即 constants[superClassIndex]，占 2 个字节
                    // 先读入 2 个字节作为方法在基类中的索引
                    index = READ_SHORT();

                    // 再读入 2 个字节作为基类在常量表中的索引
                    uint16_t superClassIndex = READ_SHORT();

                    // 然后从常量表中取出该基类
                    class = VALUE_TO_CLASS(fn->constants.datas[superClassIndex]);

                    // 最后从基类的 methods 即 class->methods 缓冲区中取出方法
                    method = &class->methods.datas[index];

                    goto invokeMethod;

                invokeMethod:
                    // 如果方法不存在，则报错
                    if ((uint32_t)index > class->methods.count || method->type == MT_NONE) {
                        RUN_ERROR("method \"%s\" not found!", vm->allMethodNames.datas[index].str);
                    }
                    switch (method->type) {
                        // 用 C 实现的原生方法
                        case MT_PRIMITIVE:
                            // 执行原生方法
                            if (method->primFn(vm, args)) {
                                // 如果返回结果为 true，说明原生方法执行正常，则回收该方法参数在运行时栈的空间
                                // argNum 减 1 是为了避免回收第一个参数 args[0]
                                // 因为被调用的方法用 args[0] 存储返回值，并由于主调方和被调方的运行时栈接壤，
                                // 所以主调方才能在自己的栈顶（即此处的 args[0]）获取被调用方法的执行结果
                                // 注意：args[0] 所在的 slot 就是 stackStart[0]，即本方法运行时栈的起始
                                curThread->esp -= argNum - 1;
                            } else {
                                // 如果返回结果为 false，则有两种情况：
                                // 1. 方法执行出错，无法运行下去（例如 primThreadAbort 使线程报错或无错退出）
                                // 2. 被调用的方法调用 Thread.yield（该方法返回 false），主动交出使用权，让下一个线程运行
                                // 总结来说就是切换线程

                                // 首先备份当前帧栈 frame 对应的指令流进度指针 ip，以便后面重新回到该帧栈时，能够从之前指令流执行的位置继续执行
                                STORE_CUR_FRAME();

                                // 如果 curThread->errorObj 不为空，说明是第 1 种情况--方法执行出错，导致切换线程的
                                if (!VALUE_IS_NULL(curThread->errorObj)) {
                                    // 直接报错
                                    if (VALUE_IS_OBJSTR(curThread->errorObj)) {
                                        ObjString *err = VALUE_TO_OBJSTR(curThread->errorObj);
                                        printf("%s", err->value.start);
                                    }
                                    // 并将该方法的错误返回值（位于第一个参数 args[0] 中，即运行时栈顶），置为 NULL
                                    PEEK() = VT_TO_VALUE(VT_NULL);
                                }

                                // 如果 vm->curThread 为 NULL，说明没有待执行的线程了，因此虚拟机执行完毕
                                if (vm->curThread == NULL) {
                                    return VM_RESULT_SUCESS;
                                }

                                // 切换到下一个线程
                                curThread = vm->curThread;

                                // 加载 curThread->frames 中最新的帧栈 frame
                                LOAD_CUR_FRAME();
                            }
                            break;

                        // 用脚本语言实现的方法
                        case MT_SCRIPT:
                            // 备份当前帧栈 frame 对应的指令流进度指针 ip
                            STORE_CUR_FRAME();
                            // 为线程 objThread 中运行的函数闭包 objClosure 准备帧栈 Frame，即闭包（函数或方法）的运行资源
                            // 该函数执行完之后，该函数创建的帧栈就是 curThread->frames 中最新的帧栈
                            createFrame(vm, curThread, (ObjClosure *)method->obj, argNum);
                            // 加载 curThread->frames 中最新的帧栈 frame
                            LOAD_CUR_FRAME();
                            break;

                        // 关于函数对象的调用方法，用于实现函数重载，如 fun1.call()
                        case MT_FN_CALL:
                            // 该类型的方法，实例对象本身就是待调用的函数（即第一个参数 args[0] 就是待调用的函数闭包）
                            ASSERT(VALUE_IS_OBJCLOSURE(args[0]), "instance must be a closure!");
                            // 备份当前帧栈 frame 对应的指令流进度指针 ip
                            STORE_CUR_FRAME();
                            // 为线程 objThread 中运行的函数闭包 objClosure 准备帧栈 Frame，即闭包（函数或方法）的运行资源
                            // 该函数执行完之后，该函数创建的帧栈就是 curThread->frames 中最新的帧栈
                            // 注意：该类型的方法，实例对象本身就是待调用的函数（即第一个参数 args[0] 就是待调用的函数闭包）
                            createFrame(vm, curThread, VALUE_TO_OBJCLOSURE(args[0]), argNum);
                            // 加载 curThread->frames 中最新的帧栈 frame
                            LOAD_CUR_FRAME();
                            break;

                        default:
                            NOT_REACHED();
                    }

                    goto loopStart;
            }

        case OPCODE_LOAD_UPVALUE:
            //【将自由变量的值（即指针 upvalue->localVarPtr 指向的局部变量的值）压入到运行时栈顶】
            // 操作数为自由变量在 upvalues 数组中的索引，占 1 个字节
            PUSH(*(curFrame->closure->upvalues[READ_BYTE()]->localVarPtr));
            goto loopStart;

        case OPCODE_STORE_UPVALUE:
            //【将运行时栈顶的值保存为自由变量的值（即指针 upvalue->localVarPtr 指向的局部变量的值）】
            // 操作数为自由变量在 upvalues 数组中的索引，占 1 个字节
            *(curFrame->closure->upvalues[READ_BYTE()]->localVarPtr) = PEEK();
            goto loopStart;

        case OPCODE_LOAD_MODULE_VAR:
            //【将模块变量的值压入到运行时栈顶】
            // 操作数为模块变量在 moduleVarValue 缓冲区中的索引，占 2 个字节
            PUSH(fn->module->moduleVarValue.datas[READ_SHORT()]);
            goto loopStart;

        case OPCODE_STORE_MODULE_VAR:
            //【将运行时栈顶的值保存为模块变量的值】
            // 操作数为模块变量在 moduleVarValue 缓冲区中的索引，占 2 个字节
            fn->module->moduleVarValue.datas[READ_SHORT()] = PEEK();
            goto loopStart;

        default:
            break;
    }
}
