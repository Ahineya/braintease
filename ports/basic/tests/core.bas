REM Cases deliberately combine control flow, locals, aliasing and text I/O.
x = 10
CALL change(x, (x + 2))
IF x <> 22 THEN PRINT "FAIL: BYREF": END
CALL localcheck()
CALL localcheck()
IF 0 THEN
    PRINT "FAIL: IF"
ELSEIF x = 22 THEN
    SELECT CASE "bee"
        CASE "aye"
            PRINT "FAIL: CASE"
        CASE "bee", "cee"
            PRINT "BRANCH_OK"
        CASE ELSE
            PRINT "FAIL: ELSE"
    END SELECT
ELSE
    PRINT "FAIL: ELSE"
END IF
sum = 0
FOR i = 5 TO 1 STEP -2
    FOR j = 1 TO 2
        sum = sum + i
    NEXT j
NEXT i
WHILE sum < 20
    sum = sum + 1
WEND
IF sum <> 20 THEN PRINT "FAIL: LOOPS": END
IF -2 ^ 2 <> -4 OR INT(-1.2) <> -2 OR FIX(-1.2) <> -1 THEN PRINT "FAIL: NUMBERS": END
IF NOT (2 < 3) THEN PRINT "FAIL: NOT": END
IF (7 AND 3) <> 3 OR (4 XOR 7) <> 3 THEN PRINT "FAIL: BITS": END
IF LEFT$(UCASE$("abc"), 2) <> "AB" THEN PRINT "FAIL: STRINGS": END
IF RIGHT$("abcd",2) <> "cd" OR MID$("abcd",2,2) <> "bc" THEN PRINT "FAIL: SUBSTRINGS": END
IF LEN("a""b\c") <> 5 THEN PRINT "FAIL: QUOTES": END
IF STR$(12) <> " 12" THEN PRINT "FAIL: STR": END
IF GRAPHICSAVAILABLE() THEN PRINT "FAIL: GRAPHICS": END
RANDOMIZE 123
r = RND
RANDOMIZE 123
IF r <> RND OR r < 0 OR r >= 1 THEN PRINT "FAIL: RANDOM": END
OPEN "basic-state.txt" FOR OUTPUT AS #1
WRITE #1, "a,""b\c", 1.25, -1234567, ""
CLOSE #1
OPEN "basic-state.txt" FOR INPUT AS #2
INPUT #2, text$, fractional, negative, empty$
CLOSE #2
IF text$ <> "a,""b\c" OR fractional <> 1.25 OR negative <> -1234567 OR empty$ <> "" THEN PRINT "FAIL: FILE": END
GOTO success
PRINT "FAIL: GOTO"
success:
PRINT "BASIC_CORE_OK"
END

SUB change(a, b)
    CALL add(a, b)
END SUB
SUB add(a, b)
    a = a + b
END SUB
SUB localcheck()
    local = local + 1
    IF local <> 1 THEN PRINT "FAIL: LOCALS": END
END SUB
