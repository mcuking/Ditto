#include <string.h>
#include <sys/stat.h>
#include "vm.h"
#include "utils.h"

// 源码文件所在的根目录，其值是在 cli.c 文件中设置的
// 解释器运行时会获得源码文件所在路径并写入 rootDir
char *rootDir = NULL;

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
    char *fileContent = (char *)molloc(fileSize + 1);
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
