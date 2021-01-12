/*
 * Unit tests for wide string functions
 *
 * Copyright 2020 Alex Henrie
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "wine/test.h"
#include "winnls.h"

static INT (WINAPI *pCompareStringW)(LCID, DWORD, const WCHAR*, INT, const WCHAR*, INT);
static INT (WINAPI *pLCMapStringW)(LCID, DWORD, const WCHAR*, INT, WCHAR*, INT);
static BOOL (WINAPI *pEnumLocales)(int, void*);

static const WCHAR hello_lower[] = L"hello";
static const WCHAR hello_upper[] = L"HELLO";

static void init_function_pointers(void)
{
    HMODULE module = LoadLibraryA("mswstr10.dll");

#define GETFUNCPTR(name, ordinal) p##name = (void*)GetProcAddress(module, (const char *)ordinal)
    GETFUNCPTR(CompareStringW, 1);
    GETFUNCPTR(LCMapStringW, 2);
    GETFUNCPTR(EnumLocales, 3);
#undef GETFUNCPTR
}

void test_CompareString(void)
{
    int ret;

    CompareStringW(LOCALE_USER_DEFAULT, 0, hello_lower, -1, hello_upper, -1);
    ret = pCompareStringW(LOCALE_USER_DEFAULT, 0, hello_lower, -1, hello_upper, -1);
    ok(ret == CSTR_LESS_THAN, "expected %d, got %d\n", CSTR_GREATER_THAN, ret);
}

void test_LCMapString(void)
{
    int ret;
    WCHAR buf[256];

    ret = pLCMapStringW(LOCALE_USER_DEFAULT, LOCALE_USE_CP_ACP | LCMAP_UPPERCASE,
                        hello_lower, -1, buf, sizeof(buf));
    ok(ret == ARRAY_SIZE(hello_upper), "expected %d, got %d\n", ARRAY_SIZE(hello_upper), ret);
    ok(!memcmp(buf, hello_upper, ret), "expected %s, got %s\n", wine_dbgstr_w(hello_upper), wine_dbgstr_w(buf));
}

BOOL CALLBACK enum_locales_callback(LCID id, const WCHAR *name)
{
    trace("0x%x => %s\n", id, wine_dbgstr_w(name));
    ok(IsValidLocale(LANGIDFROMLCID(id), LCID_SUPPORTED), "0x%x is not a valid locale\n", id);
    return TRUE;
}

void test_EnumLocales(void)
{
    BOOL ret;
    int i;

    ret = pEnumLocales(0, NULL);
    ok(ret == TRUE, "expected TRUE, got %d\n", ret);

    for (i = 0; (ret = pEnumLocales(i, enum_locales_callback)); i++)
    {
        ok(ret == TRUE, "expected TRUE, got %d\n", ret);
    }
}

START_TEST(wstr)
{
    init_function_pointers();
    test_CompareString();
    test_LCMapString();
    test_EnumLocales();
}
