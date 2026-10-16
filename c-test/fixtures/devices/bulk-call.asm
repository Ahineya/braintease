; Tail-call the uploaded code, retaining the C caller's RA/RAB continuation.
bulk_call:
    JALR R0, A0, A1
