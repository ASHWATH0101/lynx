#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bava.h"
#include "lynx.h"

static double now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
#define N 2000000

/* ---------- BAVA: A -> B write, B -> A ack ---------- */
static uint8_t bq_ab[512], bq_ba[512]; static size_t bn_ab, bn_ba; static size_t bava_wire;
static void bava_txA(const uint8_t*d,uint16_t n){memcpy(bq_ab+bn_ab,d,n);bn_ab+=n;bava_wire+=n;}
static void bava_txB(const uint8_t*d,uint16_t n){memcpy(bq_ba+bn_ba,d,n);bn_ba+=n;bava_wire+=n;}
static bava_handle_t BA, BB;

/* ---------- LYNX ---------- */
static uint8_t lq_ab[512], lq_ba[512]; static size_t ln_ab, ln_ba; static size_t lynx_wire;
static void lynx_tx(void*c,const uint8_t*d,uint16_t n){ if(c){memcpy(lq_ab+ln_ab,d,n);ln_ab+=n;} else {memcpy(lq_ba+ln_ba,d,n);ln_ba+=n;} lynx_wire+=n;}
static lx_t LA, LB;

int main(void){
    static int32_t a1,b1; static float a2,b2; static uint8_t a3,b3; static uint32_t a4,b4;
    /* BAVA */
    bava_init(&BA,bava_txA); bava_init(&BB,bava_txB);
    bava_register_var(&BA,1,&a1,4); bava_register_var(&BB,1,&b1,4);
    bava_register_var(&BA,2,&a2,4); bava_register_var(&BB,2,&b2,4);
    bava_register_var(&BA,3,&a3,1); bava_register_var(&BB,3,&b3,1);
    bava_register_var(&BA,4,&a4,4); bava_register_var(&BB,4,&b4,4);
    /* LYNX */
    lx_init(&LA,lynx_tx,NULL,(void*)1); lx_init(&LB,lynx_tx,NULL,NULL);
    lx_bind(&LA,1,&a1,4); lx_bind(&LB,1,&b1,4); lx_bind(&LA,2,&a2,4); lx_bind(&LB,2,&b2,4);
    lx_bind(&LA,3,&a3,1); lx_bind(&LB,3,&b3,1); lx_bind(&LA,4,&a4,4); lx_bind(&LB,4,&b4,4);

    /* Test 1: reliable single 4-byte write + ACK round trip */
    long okb=0, okl=0; double t0,t1;
    bava_wire=0; t0=now_ns();
    for(long i=0;i<N;i++){ a1=(int32_t)i; bava_tick(&BA,(uint32_t)i); bava_send_write(&BA,1);
        for(size_t k=0;k<bn_ab;k++)bava_process_byte(&BB,bq_ab[k]); bn_ab=0;
        for(size_t k=0;k<bn_ba;k++)bava_process_byte(&BA,bq_ba[k]); bn_ba=0;
        okb += (b1==(int32_t)i); }
    t1=now_ns(); double bava_ns=(t1-t0)/N; size_t bava_bpf=bava_wire/N;

    lynx_wire=0; t0=now_ns();
    for(long i=0;i<N;i++){ a1=(int32_t)i; lx_tick(&LA,(uint32_t)i); lx_write(&LA,LX_BIT(1));
        lx_rx_buf(&LB,lq_ab,ln_ab); ln_ab=0; lx_rx_buf(&LA,lq_ba,ln_ba); ln_ba=0;
        okl += (b1==(int32_t)i); }
    t1=now_ns(); double lynx_ns=(t1-t0)/N; size_t lynx_bpf=lynx_wire/N;
    printf("1) reliable 4B write+ACK : BAVA %6.1f ns, %zu B on wire, ok %ld/%d | LYNX %6.1f ns, %zu B, ok %ld/%d\n",
        bava_ns,bava_bpf,okb,N,lynx_ns,lynx_bpf,okl,N);

    /* Test 2: sync 4 variables (13 bytes of data), reliable */
    okb=okl=0; bava_wire=0; t0=now_ns();
    for(long i=0;i<N/4;i++){ a1=(int32_t)i;a2=(float)i;a3=(uint8_t)i;a4=(uint32_t)i;
        for(uint8_t id=1;id<=4;id++){ bava_tick(&BA,(uint32_t)(i*4+id)); bava_send_write(&BA,id);
            for(size_t k=0;k<bn_ab;k++)bava_process_byte(&BB,bq_ab[k]); bn_ab=0;
            for(size_t k=0;k<bn_ba;k++)bava_process_byte(&BA,bq_ba[k]); bn_ba=0; }
        okb += (b1==(int32_t)i && b4==(uint32_t)i); }
    t1=now_ns(); bava_ns=(t1-t0)/(N/4); bava_bpf=bava_wire/(N/4);
    lynx_wire=0; t0=now_ns();
    for(long i=0;i<N/4;i++){ a1=(int32_t)i;a2=(float)i;a3=(uint8_t)i;a4=(uint32_t)i;
        lx_tick(&LA,(uint32_t)i); lx_write(&LA,LX_BIT(1)|LX_BIT(2)|LX_BIT(3)|LX_BIT(4));
        lx_rx_buf(&LB,lq_ab,ln_ab); ln_ab=0; lx_rx_buf(&LA,lq_ba,ln_ba); ln_ba=0;
        okl += (b1==(int32_t)i && b4==(uint32_t)i); }
    t1=now_ns(); lynx_ns=(t1-t0)/(N/4); lynx_bpf=lynx_wire/(N/4);
    printf("2) 4 vars reliable       : BAVA %6.1f ns, %zu B on wire, ok %ld/%d | LYNX %6.1f ns, %zu B, ok %ld/%d\n",
        bava_ns,bava_bpf,okb,N/4,lynx_ns,lynx_bpf,okl,N/4);

    /* Test 3: telemetry stream 4B, no ACK needed (LYNX publish) */
    lynx_wire=0; okl=0; t0=now_ns();
    for(long i=0;i<N;i++){ a1=(int32_t)i; lx_publish(&LA,LX_BIT(1)); lx_rx_buf(&LB,lq_ab,ln_ab); ln_ab=0; okl+=(b1==(int32_t)i);}
    t1=now_ns();
    printf("3) 4B telemetry, no ACK  : BAVA   (no such mode)          | LYNX %6.1f ns, %zu B, ok %ld/%d\n",(t1-t0)/N,lynx_wire/N,okl,N);
    printf("RAM: sizeof(bava_handle_t)=%zu  sizeof(lx_t)=%zu (LX_MAX_PAYLOAD=%u vs BAVA %u)\n",sizeof(bava_handle_t),sizeof(lx_t),LX_MAX_PAYLOAD,BAVA_MAX_PAYLOAD);
    return 0;
}
