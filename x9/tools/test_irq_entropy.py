"""Exercise the X9 IRQ admission rule with actual XNU filter/health tests."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/out/pan56-irq-entropy-fix/kernel-source'


def function(text, signature):
    a = text.index(signature)
    return text[a:text.index('\n}', a) + 2]


@unittest.skipUnless(shutil.which('clang'), 'requires clang')
class IRQEntropyTests(unittest.TestCase):
    def test_scheduled_irq_admission_and_repetition_failure_without_disabling_tests(self):
        entropy = (SOURCE / 'osfmk/prng/entropy.c').read_text()
        constants = '\n'.join(re.findall(r'^#define (?:ENTROPY_FILTER_THRESHOLD|REPETITION_COUNT_BOUND|ADAPTIVE_PROPORTION_BOUND|ADAPTIVE_PROPORTION_WINDOW|ADAPTIVE_PROPORTION_INDEX_MASK)\b.*$', entropy, re.M))
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define __assert_only
#define __improbable(x) (x)
#define MAX(a,b) ((a)>(b)?(a):(b))
typedef unsigned uint;
typedef uint32_t entropy_sample_t;
typedef uint64_t bitmap_t;
#define BITMAP_LEN(n) (((n)+63u)/64u)
static void bitmap_zero(bitmap_t *b,unsigned n) {memset(b,0,BITMAP_LEN(n)*sizeof(*b));}
static void bitmap_set(bitmap_t *b,unsigned n) {b[n/64]|=1ULL<<(n%64);}
static int bitmap_test(const bitmap_t *b,unsigned n) {return (int)((b[n/64]>>(n%64))&1);}
typedef struct {uint32_t reset_count,failure_count,max_observation_count;} entropy_health_stats_t;
typedef struct {entropy_sample_t init_observation;uint64_t observation_count;entropy_health_stats_t *stats;} entropy_health_test_t;
typedef enum {health_test_failure,health_test_success} health_test_result_t;
static entropy_health_stats_t rct,apt;
static struct {uint64_t total_sample_count;entropy_health_test_t repetition_count_test,adaptive_proportion_test;} entropy_data;
''' + constants + '\n' + (SOURCE / 'osfmk/arm64/sweet_irq_entropy.h').read_text() + '\n'
        for signature, result in [
            ('entropy_filter(uint32_t sample_count', 'uint32_t'),
            ('add_observation(entropy_health_test_t *t', 'static health_test_result_t'),
            ('reset_test(entropy_health_test_t *t', 'static void'),
            ('repetition_count_test(entropy_sample_t observation)', 'static health_test_result_t'),
            ('adaptive_proportion_test(entropy_sample_t observation', 'static health_test_result_t'),
            ('entropy_health_test(uint32_t sample_count', 'static health_test_result_t')]:
            fixture += result + '\n' + function(entropy, signature) + '\n'
        fixture += r'''
static void reset_health(void) {
 memset(&entropy_data,0,sizeof(entropy_data));memset(&rct,0,sizeof(rct));memset(&apt,0,sizeof(apt));
 entropy_data.repetition_count_test.stats=&rct;entropy_data.repetition_count_test.init_observation=UINT32_MAX;
 entropy_data.adaptive_proportion_test.stats=&apt;entropy_data.adaptive_proportion_test.init_observation=UINT32_MAX;
}
int main(void) {
 for(unsigned i=0;i<16;i++)assert(!sweet_irq_entropy_allowed(1,i,27));
 assert(!sweet_irq_entropy_allowed(1,27,27));
 assert(sweet_irq_entropy_allowed(1,40,27));
 for(unsigned i=0;i<1020;i++)assert(sweet_irq_entropy_allowed(0,i,27));
 entropy_sample_t samples[64];bitmap_t filter[1];uint32_t time=0;
 /* Synthetic scheduled timestamps with variable intervals but a constant
  * low byte: the second-difference filter alone cannot certify entropy. */
 for(unsigned i=0;i<64;i++){time+=0x10000u+((i&1)?256u:512u);samples[i]=time;}
 unsigned accepted=entropy_filter(64,samples,1,filter);assert(accepted==64);
 reset_health();assert(entropy_health_test(64,samples,1,filter)==health_test_failure);
 assert(rct.failure_count>0);
 /* The same bad external-device samples must still fail health tests. */
 assert(sweet_irq_entropy_allowed(1,40,27));reset_health();
 assert(entropy_health_test(64,samples,1,filter)==health_test_failure);
 /* Timer/IPI exclusion does not fake passing data or alter thresholds. */
 unsigned admitted=0;
 for(unsigned i=0;i<64;i++)if(sweet_irq_entropy_allowed(1,i&1?27:1,27))admitted++;
 assert(admitted==0);
 reset_health();bitmap_t varied_filter[1];
 for(unsigned i=0;i<64;i++)samples[i]=i;
 bitmap_zero(varied_filter,64);for(unsigned i=0;i<64;i++)bitmap_set(varied_filter,i);
 assert(entropy_health_test(64,samples,1,varied_filter)==health_test_success);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='x9-entropy-test-') as d:
            p, exe = Path(d) / 'fixture.c', Path(d) / 'fixture'
            p.write_text(fixture)
            subprocess.run([shutil.which('clang'), '-std=c11', '-Wall', '-Wextra', '-Werror', str(p), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))

    def test_prng_still_panics_on_failed_entropy_and_uses_real_boot_seed(self):
        prng = (SOURCE / 'osfmk/prng/prng_random.c').read_text()
        refresh = function(prng, 'sweet_prng_refresh(struct cckprng_ctx *ctx)')
        self.assertIn('if (nsamples < 0)', refresh)
        self.assertIn('panic("SWEET PRNG entropy health test failed:', refresh)
        self.assertIn('entropy_health_rct_stats.failure_count', refresh)
        self.assertIn('entropy_health_apt_stats.failure_count', refresh)
        self.assertIn('PE_get_random_seed(seed, sizeof(seed))', prng)
        sleh = (SOURCE / 'osfmk/arm64/sleh.c').read_text()
        self.assertIn('sweet_irq_entropy_allowed(sweet_early_is_x9(), intid, sweet_vtimer_intid)', sleh)


if __name__ == '__main__': unittest.main()
