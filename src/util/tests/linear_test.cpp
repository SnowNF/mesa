/*
 * Copyright © 2023 Intel Corporation.
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include "util/ralloc.h"

TEST(LinearAlloc, Basic)
{
   void *ctx = ralloc_context(NULL);
   linear_ctx *lin_ctx = linear_context(ctx);

   for (unsigned i = 0; i < 1024; i++) {
      linear_alloc_child(lin_ctx, i * 4);
   }

   ralloc_free(ctx);
}

TEST(LinearAlloc, RallocParent)
{
   void *ctx = ralloc_context(NULL);
   linear_ctx *lin_ctx = linear_context(ctx);
   EXPECT_EQ(ralloc_parent_of_linear_context(lin_ctx), ctx);
   ralloc_free(ctx);
}

TEST(LinearAlloc, StrCat)
{
   void *ctx = ralloc_context(NULL);
   linear_ctx *lin_ctx = linear_context(ctx);

   char *s = linear_strdup(lin_ctx, "hello,");
   bool ok = linear_strcat(lin_ctx, &s, " triangle");
   EXPECT_TRUE(ok);
   EXPECT_STREQ(s, "hello, triangle");

   ralloc_free(ctx);
}

TEST(LinearAlloc, RewriteTail)
{
   void *ctx = ralloc_context(NULL);
   linear_ctx *lin_ctx = linear_context(ctx);

   char *s = linear_strdup(lin_ctx, "hello, world");
   size_t start = 7;
   bool ok = linear_asprintf_rewrite_tail(lin_ctx, &s, &start, "%s", "triangle");
   EXPECT_TRUE(ok);
   EXPECT_STREQ(s, "hello, triangle");
   EXPECT_EQ(start, 7 + 8);

   ralloc_free(ctx);
}