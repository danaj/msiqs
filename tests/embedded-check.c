#include "../embedded/siqs.c"
#include <stdio.h>
static void check_lanczos(void) {
  unsigned long rows=2000,cols=2064,c,r;int mode;
  la_col_t *columns=(la_col_t *)calloc(cols,sizeof(*columns));
  uint64_t *parity=(uint64_t *)calloc(rows,sizeof(*parity));
  if(columns==NULL||parity==NULL)exit(20);
  for(c=0;c<cols;c++) {
    columns[c].weight=3;columns[c].orig=(uint32_t)c;
    columns[c].data=(uint32_t *)malloc(3*sizeof(uint32_t));
    if(columns[c].data==NULL)exit(21);
    columns[c].data[0]=(uint32_t)(c%rows);
    columns[c].data[1]=(uint32_t)((c+1)%rows);
    columns[c].data[2]=(uint32_t)((c+7)%rows);
  }
  for(mode=0;mode<2;mode++) {
    uint64_t mask=0,*dependencies=mode?
      la_block_lanczos_wide(rows,0,cols,columns,UINT64_C(0xc3f9821d4076ab5e),&mask,0):
      la_block_lanczos(rows,0,cols,columns,0,&mask,0);
    if(dependencies==NULL||mask==0)exit(22);
    memset(parity,0,rows*sizeof(*parity));
    for(c=0;c<cols;c++)for(r=0;r<columns[c].weight;r++)
      parity[columns[c].data[r]]^=dependencies[c]&mask;
    for(r=0;r<rows;r++)if(parity[r]!=0)exit(23);
    free(dependencies);
  }
  for(c=0;c<cols;c++)free(columns[c].data);
  free(columns);free(parity);
  puts("PASS embedded Lanczos: normal/wide sparse solvers, independent GF(2) verification");
}
int main(void) {
  check_lanczos();
  const char *inputs[]={"1","2","77","1000000016000000063",
    "22095311209999409685885162322219",
    "713623846352979940529142984724747568191373381"};
  unsigned i;mpz_t n,p;mpz_init(n);mpz_init(p);
  for(i=0;i<sizeof(inputs)/sizeof(inputs[0]);i++) {
    uint32_t count,j;mpz_t *f;mpz_set_str(n,inputs[i],10);
    f=gmp_siqs(n,&count,0,0);mpz_set_ui(p,1);
    for(j=0;j<count;j++){if(mpz_cmp_ui(n,1)>0 && mpz_cmp_ui(f[j],1)<=0)return 1;mpz_mul(p,p,f[j]);}
    if(mpz_cmp(p,n))return 2;gmp_siqs_free(f,count);
  }
  {
    const char *fixtures[]={"597107386758137","610235247872417","1330578890360873","2941303423812353","39586268787172817"};
    const char *profiles[]={"one_lp_k60_q3_recovery","one_lp_k60_q2_geometry_recovery"};
    unsigned k,v;mpz_t divisor,root;mpz_init(divisor);mpz_init(root);
    for(k=0;k<2;k++) {
      const siqs_policy_band_t *profile=NULL;
      for(v=0;v<SIQS_RECOVERY_POLICY_COUNT;v++)
        if(!strcmp(siqs_recovery_policies[v].name,profiles[k]))profile=&siqs_recovery_policies[v];
      if(profile==NULL)return 3;
      for(v=(k==0?4:0);v<5;v++) {
        siqs_factor_array_t partition;uint32_t count,j;mpz_t *f;
        mpz_set_str(n,fixtures[v],10);siqs_factor_array_init(&partition,n);
        if(!siqs_try_policy(n,n,&partition,profile,divisor,root,0))return 4;
        f=siqs_factor_array_release(&partition,&count);mpz_set_ui(p,1);
        if(count!=2)return 5;
        for(j=0;j<count;j++){if(!mpz_probab_prime_p(f[j],25))return 6;mpz_mul(p,p,f[j]);}
        if(mpz_cmp(p,n))return 7;gmp_siqs_free(f,count);
      }
    }
    mpz_clear(divisor);mpz_clear(root);
    puts("PASS embedded SIQS: explicit q3 and terminal q2 recovery fixtures");
  }
  mpz_clear(n);mpz_clear(p);puts("PASS embedded SIQS: serial multiplicative partitions and ownership");return 0;
}
