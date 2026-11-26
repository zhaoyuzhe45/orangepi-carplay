"""Replay real reliable ACKs against the production authentication gate."""
import unittest
from ncm_compat_test import ROOT, function, run_c


class AuthenticationGateTest(unittest.TestCase):
    def test_only_acknowledged_authentication_opens_gate(self):
        source = (ROOT / 'src/iap2session.c').read_text()
        helper = function(source, 'int iap2_session_authentication_ready(')
        code = r'''
#include "iap2session.h"
#include "iap2link.c"
#define main reliability_suite_main
''' + '#include "' + str(ROOT / 'tests/iap2_link_reliability_test.c') + '"\n' + r'''
#undef main
''' + helper + r'''
int main(void) {
    struct iap2_link l;
    struct iap2_session s = {0};
    const uint8_t aa05[] = {0x40,0x40,0,6,0xaa,5};
    uint8_t wire[64];
    reliable(&l, 2);
    CHECK(iap2_link_send_control(&l, aa05, sizeof(aa05), 0) == 0);
    s.authenticated = s.authentication_reported = 1;
    s.authentication_sequence = (uint8_t)l.sent_seq;
    CHECK(!iap2_session_authentication_ready(&s, &l));
    CHECK(iap2_link_take(&l, wire, sizeof(wire)) == 16);
    CHECK(!iap2_session_authentication_ready(&s, &l)); /* queue drain is not ACK */
    acknowledgement(&l, 99, 1);
    CHECK(!iap2_session_authentication_ready(&s, &l)); /* stale */
    acknowledgement(&l, 101, 2);
    CHECK(!iap2_session_authentication_ready(&s, &l)); /* future */
    iap2_link_tick(&l, 100);
    CHECK(!iap2_session_authentication_ready(&s, &l)); /* retransmission */
    CHECK(iap2_link_send_control(&l, retry_csm, sizeof(retry_csm), 101) == 0);
    acknowledgement(&l, 100, 102);
    CHECK(l.pending_count == 1 && l.pending[0].sequence == 101);
    CHECK(iap2_session_authentication_ready(&s, &l)); /* no wait for unrelated service */
    l.state = IAP2_DEAD;
    CHECK(!iap2_session_authentication_ready(&s, &l));
    printf("real ACK authentication gate: %d failures\n", failures);
    return failures != 0;
}
'''
        result = run_c(code)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
