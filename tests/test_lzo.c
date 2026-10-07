#include "compress_utils.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t read_fixture(const char* name, unsigned char* output) {
 char path[1024];snprintf(path,sizeof(path),"%s/%s.bin",LZO_FIXTURE_DIRECTORY,name);
 FILE* file=fopen(path,"rb");assert(file);size_t length=fread(output,1,40000,file);fclose(file);return length;
}
int main(void) {
 unsigned char input[40000], expected[40000], output[40000];size_t expected_length=read_fixture("input",expected);
 const char* variants[]={"1","999"};
 for(size_t variant=0;variant<2;variant++) {
  size_t length=read_fixture(variants[variant],input), capacity=sizeof(output), hint=0;
  assert(cu_decompress_size_hint(CU_ALGO_LZO,input,length,&hint)==CU_OK && hint==expected_length);
  assert(cu_decompress(CU_ALGO_LZO,input,length,output,&capacity)==CU_OK);
  assert(capacity==expected_length && !memcmp(output,expected,capacity));
  capacity=expected_length-1;assert(cu_decompress(CU_ALGO_LZO,input,length,output,&capacity)==CU_ERR_BUF_TOO_SMALL);
  for(size_t truncated=0;truncated<length;truncated++){capacity=sizeof(output);assert(cu_decompress(CU_ALGO_LZO,input,truncated,output,&capacity)!=CU_OK);}
  input[length]=0;capacity=sizeof(output);assert(cu_decompress(CU_ALGO_LZO,input,length+1,output,&capacity)==CU_ERR_DECOMPRESSION);
 }
 unsigned char empty[]={17,0,0}, invalid[]={17,4,0};size_t capacity=0;
 assert(cu_decompress(CU_ALGO_LZO,empty,sizeof(empty),NULL,&capacity)==CU_OK && !capacity);
 capacity=sizeof(output);assert(cu_decompress(CU_ALGO_LZO,invalid,sizeof(invalid),output,&capacity)==CU_ERR_DECOMPRESSION);
 cu_compress_stream_t* compressor=NULL;cu_decompress_stream_t* decompressor=NULL;
 assert(cu_compress_bound(10,CU_ALGO_LZO)==0);
 assert(cu_compress(CU_ALGO_LZO,empty,3,output,&capacity,5)==CU_ERR_UNSUPPORTED_ALGO);
 assert(cu_compress_stream_create(CU_ALGO_LZO,5,&compressor)==CU_ERR_UNSUPPORTED_ALGO && !compressor);
 assert(cu_decompress_stream_create(CU_ALGO_LZO,&decompressor)==CU_ERR_UNSUPPORTED_ALGO && !decompressor);
 unsigned random=42;cu_set_max_decompressed_size(4096);
 for(size_t iteration=0;iteration<10000;iteration++) {
  size_t length=iteration%512;for(size_t index=0;index<length;index++){random=random*1664525+1013904223;input[index]=random>>24;}
  capacity=4096;cu_decompress(CU_ALGO_LZO,input,length,output,&capacity);size_t hint;cu_decompress_size_hint(CU_ALGO_LZO,input,length,&hint);
 }
 puts("LZO oracle, boundary, unsupported-operation and bounded fuzz tests passed");return 0;
}
