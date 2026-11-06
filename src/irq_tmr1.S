    .syntax unified
    .thumb
    .text

    .global TMR1_BRK_OVF_TRG_HALL_IRQHandler
    .thumb_func
    .type TMR1_BRK_OVF_TRG_HALL_IRQHandler, %function
TMR1_BRK_OVF_TRG_HALL_IRQHandler:
    b sm_fast_update_irq
    .size TMR1_BRK_OVF_TRG_HALL_IRQHandler, . - TMR1_BRK_OVF_TRG_HALL_IRQHandler
