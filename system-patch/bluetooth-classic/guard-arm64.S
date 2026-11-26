// Exact firmware: D33810D95EF68EB05CEF71144B25BCF4C84117F5B83755D5324C97EB927F1077.
// Replaces only the debug-log block at 0x91318 in btif_dm_upstreams_evt's
// successful BLE-auth completion branch. Keys have already been saved.
// The original LE-only discovery path resumes at 0x9133c when no BR ACL exists.
// w19=BT_BOND_STATE_BONDED is required at the common continuation, 0x9136c.
.text
.global guard
.extern BTM_IsAclConnectionUp
.extern bonded_continuation
guard:
    sub x0, x29, #112
    mov w1, #1                 // BT_TRANSPORT_BR_EDR
    bl BTM_IsAclConnectionUp
    mov w19, #2                // BT_BOND_STATE_BONDED
    cbnz w0, bonded_continuation
    nop
    nop
    nop
    nop
