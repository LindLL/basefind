#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <malloc.h>
// 指针的长度
#define POINTER_SIZE 4

// 用于存储每个字符串的起始位置
uint32_t *strings = NULL;
// 用于存储每一个指针
uint32_t *pointers = NULL;
// 字符串的数量，指针的数量
uint32_t num_strings = 0, num_pointers = 0;
// 字符串差异数量，指针差异数量
size_t strings_num_diffs = 0, pointers_num_diffs = 0;
// 字符串差异数组指针
uint32_t *str_diff = NULL;
// 指针差异数组指针
uint32_t *ptr_diff = NULL;

pthread_rwlock_t rwlock;

// 线程参数
struct thread_data
{
    int thread_id;       // 线程id
    uint32_t block_size; // 每个线程要处理的数组大小
    int difflength;
    int samplerate;
};

// 偏移量数据
struct offset_data
{
    uint32_t offset; // 偏移量的值
    uint32_t times;  // 偏移量出现的次数
};

// 偏移量数组
struct offset_data *found = NULL;
// 偏移量个数
uint32_t found_num = 0;

/// @brief 用于获取文件中所有的可见字符串
/// @param file 要进行获取的文件
/// @param min_length 字符串的最小长度
/// @param file_size 文件的大小
/// @param out_strings 用于存放各个字符串起始位置
/// @return 字符串的数量
uint32_t get_string(char *file, size_t min_length, size_t file_size, uint32_t **out_strings)
{
    uint32_t nums_strings = 0;
    uint32_t *strings = NULL;
    for (int i = 0; i < file_size - min_length; i++)
    {
        if (!isprint(file[i]))
        {
            continue;
        }
        int j = i + 1;
        while (j < file_size && isprint(file[j]))
            j++;
        if (j - i >= min_length)
        {
            if (nums_strings % 16 == 0)
            {
                strings = (uint32_t *)realloc(strings, (nums_strings + 16) * sizeof(uint32_t));
                if (!strings)
                {
                    printf("Failed to allocate memory for output strings\n");
                    free(strings);
                    return -1;
                }
            }
            strings[nums_strings++] = i;
        }
        i = j + 1;
    }
    *out_strings = strings;
    return nums_strings;
}

int cmp_uint32_t(const void *a, const void *b)
{
    uint32_t ua = *(const uint32_t *)a;
    uint32_t ub = *(const uint32_t *)b;
    if (ua < ub)
        return -1;
    else if (ua > ub)
        return 1;
    else
        return 0;
}

/// @brief 对offset_data按照times降序排序
/// @param a
/// @param b
/// @return
int cmp_offset_times(const void *a, const void *b)
{
    struct offset_data *data1 = (struct offset_data *)a;
    struct offset_data *data2 = (struct offset_data *)b;
    if (data1->times < data2->times)
        return 1;
    else if (data1->times > data2->times)
        return -1;
    else
        return 0;
}

/// @brief 获取文件中的所有指针，即将文件的每一个四字节都当作一个地址，然后按照升序排列
/// @param file 要获取指针的文件
/// @param file_size 文件的大小
/// @param num_pointers 用于存放指针的数量
/// @return 一个存放着所有指针的数组
uint32_t *get_pointers(uint8_t *file, size_t file_size, uint32_t *num_pointers)
{
    size_t max_offset = file_size - 4;
    uint32_t *pointers = (uint32_t *)malloc(max_offset / POINTER_SIZE * sizeof(uint32_t));
    if (!pointers)
    {
        printf("Failed to allocate memory for pointers array.\n");
        *num_pointers = 0;
        return NULL;
    }
    *num_pointers = 0;
    for (size_t offset = 0; offset < max_offset; offset += POINTER_SIZE)
    {
        uint32_t ptr = *((uint32_t *)(file + offset));
        pointers[*num_pointers] = ptr;
        (*num_pointers)++;
    }
    qsort(pointers, *num_pointers, sizeof(uint32_t), cmp_uint32_t);
    uint32_t *new_pointers = (uint32_t *)malloc((*num_pointers) * sizeof(uint32_t));
    if (!new_pointers)
    {
        printf("Failed to reallocate memory for new_pointers array.\n");
        free(pointers);
        *num_pointers = 0;
        return NULL;
    }
    printf("Pointer deduplication\n");
    int j = 0;
    for (int i = 0; i < *num_pointers; i++)
    {
        if (i == 0 || pointers[i] != pointers[i - 1])
        {
            new_pointers[j++] = pointers[i];
        }
    }
    *num_pointers = j;
    free(pointers);
    uint32_t *result = (uint32_t *)realloc(new_pointers, (*num_pointers) * sizeof(uint32_t));
    if (!result)
    {
        printf("Failed to reallocate memory for result array.\n");
        free(new_pointers);
        *num_pointers = 0;
        return NULL;
    }
    return result;
}

/// @brief 用于获取一个数组的差异数组，将数组中相邻的两个值相减然后将差值作为新的数组
/// @param ptrs 要获取差异数组的数组
/// @param num_ptrs 数组的长度
/// @param num_diffs 差异数组的长度
/// @return 差异数组的指针
uint32_t *get_difference(uint32_t *ptrs, size_t num_ptrs, size_t *num_diffs)
{
    if (!ptrs || num_ptrs == 0)
    {
        printf("Invalid input: ptrs is NULL or num_ptrs is 0.\n");
        *num_diffs = 0;
        return NULL;
    }
    // printf("%ld\n", num_ptrs);
    uint32_t *difference = (uint32_t *)malloc((num_ptrs) * sizeof(uint32_t));
    memset(difference, '\x00', (num_ptrs - 1) * sizeof(uint32_t));
    if (!difference)
    {
        printf("Failed to allocate memory for difference array.\n");
        *num_diffs = 0;
        return NULL;
    }

    uint32_t last = 0;
    *num_diffs = 0;
    for (size_t i = 0; i < num_ptrs; i++)
    {
        if (ptrs[i] < last)
        {
            printf("Invalid input: ptrs is not sorted in ascending order.\n");
            free(difference);
            *num_diffs = 0;
            return NULL;
        }
        difference[*num_diffs] = ptrs[i] - last;
        (*num_diffs)++;
        last = ptrs[i];
    }
    return difference;
}

/// @brief 用于记录某个基地址出现的次数
/// @param ptrs 存放着文件所有指针的数组
/// @param strs 存放着文件所有字符串起始位置的数组
/// @param offset 可能的加载基地址
/// @param samplerate 查找间隔
/// @param num_ptr 指针数组的长度
/// @param num_str 字符串数组的长度
/// @return 加载基地址出现的次数
uint32_t count_str(uint8_t *ptrs, uint32_t *strs, uint32_t offset, uint32_t samplerate, uint32_t num_ptr, uint32_t num_str)
{
    uint32_t c = 0;
    uint32_t last_ptr = 0;
    for (int si = 0; si < num_ptr; si += samplerate)
    {
        if ((num_ptr * sizeof(uint32_t) - last_ptr) > 0 && si < num_str)
        {
            uint32_t value = strs[si] + offset;
            pthread_rwlock_rdlock(&rwlock);
            uint8_t *ptr = (uint8_t *)memmem(&ptrs[last_ptr], num_ptr * sizeof(uint32_t) - last_ptr, &value, sizeof(uint32_t));
            pthread_rwlock_unlock(&rwlock);
            if (ptr == NULL)
                continue;
            last_ptr = ptr - ptrs;
            c += 1;
        }
    }
    return c * samplerate;
}

/// @brief 用于获取程序加载地址
/// @param arg 参数为thread_data类型
/// @return
void *find_base(void *arg)
{
    struct thread_data *data = (struct thread_data *)arg;
    uint32_t start = data->thread_id * data->block_size;
    uint32_t end = (data->thread_id + 1) * data->block_size;
    if (end > (strings_num_diffs - data->difflength))
        end = (strings_num_diffs - data->difflength);

    uint8_t *ptr_diff_b = (uint8_t *)ptr_diff;
    uint8_t *ptr_b = (uint8_t *)pointers;
    struct offset_data tmp;

    for (int si = start; si < end; si++)
    {
        int pi = -1;
        uint32_t is_found = 0;
        uint8_t *str_diff_b = (uint8_t *)&str_diff[si];
        uint8_t *ptr = (uint8_t *)memmem(ptr_diff_b, pointers_num_diffs * sizeof(uint32_t), str_diff_b, (data->difflength));
        if (ptr != NULL)
        {
            pi = (int)(ptr - ptr_diff_b);
        }
        if (pi == -1)
            continue;
        pi /= sizeof(uint32_t);
        uint32_t offset = pointers[pi] - strings[si];

        if (offset < 0 || (offset % 4 != 0))
            continue;
        uint32_t times = count_str(ptr_b, strings, offset, data->samplerate, num_pointers, num_strings);
        tmp.offset = offset;
        tmp.times = times;
        for (int i = 0; i < found_num; i++)
        {
            if (found[i].offset == offset)
            {
                pthread_rwlock_rdlock(&rwlock);
                is_found = 1;
                found[i].times += times;
                printf("Thread%d :possible offset \033[36m\033[1m0x%x\033[0m\033[0m \033[32m\033[1m%d\033[0m\033[0m times\n", data->thread_id, found[i].offset, found[i].times);
                pthread_rwlock_unlock(&rwlock);
                break;
            }
        }
        if (!is_found)
        {
            pthread_rwlock_rdlock(&rwlock);
            found[found_num] = tmp;
            printf("Thread%d :possible offset 0x%x %d times\n", data->thread_id, found[found_num].offset, found[found_num].times);
            found_num++;
            pthread_rwlock_unlock(&rwlock);
        }

        if (found_num % 16 == 0)
        {
            pthread_rwlock_rdlock(&rwlock);
            found = (struct offset_data *)realloc(found, (found_num + 16) * sizeof(struct offset_data));
            pthread_rwlock_unlock(&rwlock);
        }
    }
    pthread_exit(NULL);
}


/// @brief 一个用于查找32位arm固件加载基地址的程序，原理为为：一个基本的定理：字符串在内存中的加载地址=固件的加载基地址+字符串在固件中的偏移量。首先提取出固件中所有的字符串，将字符串在固件中的偏移量作为\
一个数组(字符串偏移量数组)，显然，这个数组是升序排列的；然后将固件中的每个4字节作为一个指针，按照升序排列的方式构成一个数组(指针数组)；然后将字符串数组中的值进行两两相减，得到一个字符串偏移量差值数组，这个数组\
表明了两两字符串之间的相对偏移；再对指针数组也做同样的处理得到一个指针差值数组，这个数组表明了指针之间的相对偏移；在指针数组之中存在着一些字符串的绝对加载地址，如果存在一些绝对加载地址是两两相邻的情况，那么\
将指针数组处理得到指针差值数组之后，这些绝对加载地址之间的差值实际上也就是它所引用的字符串偏移量之间的差值，这些差值是可以在字符串偏移量差值数组中有所体现的。所以，可以在指针差值数组之中按照一定的长度和字符串偏移量差值数组进行匹配\
如果匹配到就用相应的指针减去字符串的偏移量得到一个可能的加载基地址，再将所有的字符串偏移量加上这个基地址得到一个确切的字符串加载地址，并在指针数组中查找这些字符串加载地址是否存在，每匹配到一次就将相应的加载基地址的匹配次数加一，\
最后输出次数排名前十或者指定排名的加载基地址
/// @param argc
/// @param argv
/// @return
int main(int argc, char *argv[])
{
    int opt;
    int strlength = 10;  // 字符串的最小长度
    int difflength = 10; // 要进行匹配的字符串长度
    int samplerate = 20; // 查找间隔
    int thread_num = 1;  // 线程数
    int output_num = 10; // 输出结果数量
    char *filename = NULL;
    FILE *fp;
    size_t file_size;
    char *buffer;

    if (argc < 2)
    {
        printf("Usage: %s [-h] [--sl/-l STRLENGTH] [--dl/-d DIFFLENGTH] [--sr/-s SAMPLERATE] [--thread/-t thread_num(default 1)] [--output_num/-o output_num] [--file/-f filename]\n", argv[0]);
        return -1;
    }

    struct option long_options[] = {
        {"sl", required_argument, NULL, 'l'},
        {"dl", required_argument, NULL, 'd'},
        {"sr", required_argument, NULL, 's'},
        {"thread", required_argument, NULL, 't'},
        {"output_num", required_argument, NULL, 'o'},
        {"file", required_argument, NULL, 'f'}};

    while ((opt = getopt_long(argc, argv, "l:d:s:t:o:f:h", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'l':
            strlength = atoi(optarg);
            break;
        case 'd':
            difflength = atoi(optarg);
            break;
        case 's':
            samplerate = atoi(optarg);
            break;
        case 't':
            thread_num = atoi(optarg);
            break;
        case 'o':
            output_num = atoi(optarg);
            break;
        case 'f':
            filename = optarg;
            break;
        case 'h':
            fprintf(stderr, "Usage: %s [-h] [--sl/-l STRLENGTH] [--dl/-d DIFFLENGTH] [--sr/-s SAMPLERATE] [--thread/-t thread_num(default 1)] [--output_num/-o output_num] [--file/-f filename]\n\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    pthread_t threads[thread_num];
    struct thread_data data[thread_num];
    pthread_rwlock_init(&rwlock, NULL);

    fp = fopen(filename, "rb");
    if (fp == NULL)
    {
        printf("open file %s error\n", filename);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    buffer = (char *)malloc(file_size);
    fread(buffer, 1, file_size, fp);
    fclose(fp);

    printf("Getting strings with length greater than %d......\n", strlength);
    clock_t start_time = clock();
    num_strings = get_string(buffer, strlength, file_size, &strings);
    clock_t end_time = clock();
    double elapsed_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;
    printf("Elapsed time: %.3f seconds\n", elapsed_time);
    if (num_strings == -1)
    {
        fprintf(stderr, "Failed to get strings from file\n");
        free(buffer);
        return -1;
    }
    printf("Found %d strings:\n", num_strings);

    num_pointers = 0;
    printf("Getting pointers......\n");
    start_time = clock();
    pointers = get_pointers(buffer, file_size, &num_pointers);
    end_time = clock();
    elapsed_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;
    printf("Elapsed time: %.3f seconds\n", elapsed_time);
    printf("Found %d pointers\n", num_pointers);

    printf("Diffing strings\n");
    str_diff = get_difference(strings, num_strings, &strings_num_diffs);
    printf("Diffing pointers\n");
    ptr_diff = get_difference(pointers, num_pointers, &pointers_num_diffs);

    printf("finding differences of length: %d\n", difflength);
    found = (struct offset_data *)malloc((found_num + 16) * sizeof(struct offset_data));
    memset(found, '\x00', malloc_usable_size(found));
    for (int i = 0; i < thread_num; i++)
    {
        data[i].thread_id = i;
        data[i].block_size = (strings_num_diffs - difflength) / thread_num;
        data[i].difflength = difflength;
        data[i].samplerate = samplerate;
        printf("Starting thread%d\n", data[i].thread_id);
        pthread_create(&threads[i], NULL, find_base, (void *)&data[i]);
    }

    for (int i = 0; i < thread_num; i++)
    {
        pthread_join(threads[i], NULL);
    }
    pthread_rwlock_destroy(&rwlock);
    qsort(found, found_num, sizeof(struct offset_data), cmp_offset_times);
    printf("\033[2J\033[1;1H");
    for (int i = 0; i < output_num; i++)
        printf("\033[0;32m%d:\033[0m offset-\033[0;36m\033[1m0x%x\033[0m\033[0m-\033[0;33m\033[1m%d\033[0m\033[0mtimes\n", i + 1, found[i].offset, found[i].times);
    free(strings);
    free(buffer);
    free(found);
    return 0;
}
