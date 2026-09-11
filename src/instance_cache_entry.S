.syntax unified
.thumb
.text
.align 2
.global instance_pool_original
.type instance_pool_original, %function
.thumb_func
instance_pool_original:
	push.w {r4-r8, lr}
	movw r12, #:lower16:g_instance_pool_resume
	movt r12, #:upper16:g_instance_pool_resume
	ldr r12, [r12]
	bx r12
.size instance_pool_original, .-instance_pool_original
.align 2
.global instance_request_original
.type instance_request_original, %function
.thumb_func
instance_request_original:
	push.w {r4-r9, lr}
	movw r12, #:lower16:g_instance_request_resume
	movt r12, #:upper16:g_instance_request_resume
	ldr r12, [r12]
	bx r12
.size instance_request_original, .-instance_request_original
