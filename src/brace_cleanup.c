/**
 * @file brace_cleanup.c
 * Determines the brace level and paren level.
 * Inserts virtual braces as needed.
 * Handles all that preprocessor crap.
 *
 * $Id: tokenize.c 74 2006-03-18 05:26:34Z bengardner $
 */

#include "cparse_types.h"
#include "char_table.h"
#include "prototypes.h"
#include "chunk_list.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>


static chunk_t *insert_vbrace(chunk_t *pc, BOOL after,
                              struct parse_frame *frm);
#define insert_vbrace_after(pc, frm)      insert_vbrace(pc, TRUE, frm)
#define insert_vbrace_before(pc, frm)     insert_vbrace(pc, FALSE, frm)

chunk_t *parse_cleanup(struct parse_frame *frm, chunk_t *pc);

void close_statement(struct parse_frame *frm, chunk_t *pc);

static BOOL check_complex_statements(struct parse_frame *frm, chunk_t *pc);
static void handle_complex_close(struct parse_frame *frm, chunk_t *pc);


static void preproc_start(struct parse_frame *frm, chunk_t *pc)
{
//   chunk_t *prev;
   chunk_t *next;

   /* Close any virtual braces - they can't cross preprocessors
    * this seems dumb!!!
    */
//   prev = chunk_get_prev_ncnl(pc);
//   if (prev != NULL)
//   {
//      if ((frm->pse[frm->pse_tos].type == CT_VBRACE_OPEN) ||
//          (frm->pse[frm->pse_tos].type == CT_IF) ||
//          (frm->pse[frm->pse_tos].type == CT_FOR) ||
//          (frm->pse[frm->pse_tos].type == CT_SWITCH) ||
//          (frm->pse[frm->pse_tos].type == CT_DO) ||
//          (frm->pse[frm->pse_tos].type == CT_WHILE) ||
//          (frm->pse[frm->pse_tos].type == CT_VOLATILE) ||
//          (frm->pse[frm->pse_tos].type == CT_BRACED))
//      {
//         //fprintf(stderr, "%s: closing on token %s\n",
//         //        __func__, get_token_name(pc->type));
//         close_statement(frm, prev);
//      }
//   }

   /* Get the type of preprocessor and handle it */
   next = chunk_get_next_ncnl(pc);
   if (next != NULL)
   {
      cpd.in_preproc = next->type;

      /**
       * If we are in a define, push the frame stack.
       */
      if (cpd.in_preproc == CT_PP_DEFINE)
      {
         pf_push(frm);

         /* a preproc body starts a new, blank frame */
         memset(frm, 0, sizeof(*frm));
         frm->level       = 1;
         frm->brace_level = 1;

         /*TODO: not sure about the next 3 lines */
         frm->pse_tos = 1;
         frm->pse[frm->pse_tos].type  = CT_PP_DEFINE;
         frm->pse[frm->pse_tos].stage = BS_NONE;
      }
      else
      {
         /* Check for #if, #else, #endif, etc */
         pf_check(frm, next);
      }
   }
}


/**
 * Scans through the whole list and does stuff.
 * It has to do some tricks to parse preprocessors.
 *
 * TODO: This can be cleaned up and simplified - we can look both forward and backward!
 */
void brace_cleanup(void)
{
   chunk_t            *pc;
   struct parse_frame frm;

   memset(&frm, 0, sizeof(frm));

   cpd.in_preproc = CT_NONE;

   pc = chunk_get_head();
   while (pc != NULL)
   {
      /* Check for leaving a #define body */
      if ((cpd.in_preproc != CT_NONE) && ((pc->flags & PCF_IN_PREPROC) == 0))
      {
         if (cpd.in_preproc == CT_PP_DEFINE)
         {
            /* out of the #define body, restore the frame */
            pf_pop(&frm);
         }

         cpd.in_preproc = CT_NONE;
      }

      /* Check for a preprocessor start */
      if (pc->type == CT_PREPROC)
      {
         preproc_start(&frm, pc);
      }

      /* Assume the level won't change */
      pc->level       = frm.level;
      pc->brace_level = frm.brace_level;

      /* #define bodies get the full formatting treatment */
      if (!chunk_is_comment(pc) && !chunk_is_newline(pc) &&
         ((cpd.in_preproc == CT_PP_DEFINE) || (cpd.in_preproc == CT_NONE)))
      {
         pc = parse_cleanup(&frm, pc);
      }
      else
      {
         pc = chunk_get_next(pc);
      }
   }
}


static void print_stack(struct parse_frame *frm, chunk_t *pc)
{
   if (log_sev_on(LFRMSTK))
   {
      int idx;

      log_fmt(LFRMSTK, "%2d> %2d", pc->orig_line, frm->pse_tos);

      for (idx = 1; idx <= frm->pse_tos; idx++)
      {
         log_fmt(LFRMSTK, " [%s/%d]",
                 get_token_name(frm->pse[idx].type), frm->pse[idx].stage);
      }
      log_fmt(LFRMSTK, "\n");
   }
}


/**
 * At the heart of this algorithm are two stacks.
 * There is the Paren Stack (PS) and the Frame stack.
 *
 * The PS (pse in the code) keeps track of braces, parens,
 * if/else/switch/do/while/etc items -- anything that is nestable.
 * Complex statements go through stages.
 * Take this simple if statment as an example:
 *   if ( x ) { x--; }
 *
 * The stack would change like so: 'token' stack afterwards
 * 'if' [IF - 1]
 * '('  [IF - 1] [PAREN OPEN]
 * 'x'  [IF - 1] [PAREN OPEN]
 * ')'  [IF - 2]       <- note that the state was incremented
 * '{'  [IF - 2] [BRACE OPEN]
 * 'x'  [IF - 2] [BRACE OPEN]
 * '--' [IF - 2] [BRACE OPEN]
 * ';'  [IF - 2] [BRACE OPEN]
 * '}'  [IF - 3]
 *                             <- lack of else kills the IF, closes statement
 *
 * Virtual braces example:
 *   if ( x ) x--; else x++;
 *
 * 'if'   [IF - 1]
 * '('    [IF - 1] [PAREN OPEN]
 * 'x'    [IF - 1] [PAREN OPEN]
 * ')'    [IF - 2]
 * 'x'    [IF - 2] [VBRACE OPEN]   <- VBrace open inserted before because '{' was not next
 * '--'   [IF - 2] [VBRACE OPEN]
 * ';'    [IF - 3]                 <- VBrace close inserted after semicolon
 * 'else' [ELSE - 0]               <- IF changed into ELSE
 * 'x'    [ELSE - 0] [VBRACE OPEN] <- lack of '{' -> VBrace
 * '++'   [ELSE - 0] [VBRACE OPEN]
 * ';'    [ELSE - 0]               <- VBrace close inserted after semicolon
 *                                 <- ELSE removed after statment close
 *
 * The pse stack is kept on a frame stack.
 * The frame stack is need for languages that support preprocessors (C, C++, C#)
 * that can arbitrarily change code flow. It also isolates #define macros so
 * that they are indented independently and do not affect the rest of the program.
 *
 * When an #if is hit, a copy of the current frame is push on the frame stack.
 * When an #else/#elif is hit, a copy of the current stack is pushed under the
 * #if frame and the original (pre-#if) frame is copied to the current frame.
 * When #endif is hit, the top frame is popped.
 * This has the following effects:
 *  - a simple #if / #endif does not affect program flow
 *  - #if / #else /#endif - continues from the #if clause
 *
 * When a #define is entered, the current frame is pushed and cleared.
 * When a #define is exited, the frame is popped.
 */
chunk_t *parse_cleanup(struct parse_frame *frm, chunk_t *pc)
{
   c_token_t parent = CT_NONE;
   chunk_t   *prev;

   LOG_FMT(LTOK, "%s:%d] %16s - tos:%d/%16s stg:%d\n",
           __func__, pc->orig_line, get_token_name(pc->type),
           frm->pse_tos, get_token_name(frm->pse[frm->pse_tos].type),
           frm->pse[frm->pse_tos].stage);


   /* Mark statement starts */
   if (((frm->stmt_count == 0) || (frm->expr_count == 0)) &&
       (pc->type != CT_SEMICOLON) &&
       (pc->type != CT_BRACE_CLOSE))
   {
      pc->flags |= PCF_EXPR_START;
      pc->flags |= (frm->stmt_count == 0) ? PCF_STMT_START : 0;
      LOG_FMT(LPCU, "%d] 1.marked %s as stmt start st:%d ex:%d\n",
              pc->orig_line, pc->str, frm->stmt_count, frm->expr_count);
   }
   frm->stmt_count++;
   frm->expr_count++;

   if (frm->sparen_count > 0)
   {
      pc->flags |= PCF_IN_SPAREN;
   }

   /**
    * Check for a virtual brace close due to semicolon
    * Virtual braces also may be added on a complex statement close.
    */
   if ((pc->type == CT_SEMICOLON) &&
       (frm->pse[frm->pse_tos].type == CT_VBRACE_OPEN))
   {
      /**
       * Insert a virtual brace (pc points to the new CT_VBRACE_CLOSE).
       * This skips the semicolon.
       * TODO: may need to float VBRACE past comments until newline?
       * TODO: reset statement and expression counters
       */
      pc = insert_vbrace_after(pc, frm);
   }

   /* Check the progression of complex statements */
   if (frm->pse[frm->pse_tos].stage != BS_NONE)
   {
      if (check_complex_statements(frm, pc))
      {
         return(chunk_get_next(pc));
      }
   }

   /* Handle close paren, vbrace, brace, and square */
   if ((pc->type == CT_PAREN_CLOSE) ||
       (pc->type == CT_BRACE_CLOSE) ||
       (pc->type == CT_VBRACE_CLOSE) ||
       (pc->type == CT_SQUARE_CLOSE))
   {
      /* Change CT_PAREN_CLOSE into CT_SPAREN_CLOSE or CT_FPAREN_CLOSE */
      if ((pc->type == CT_PAREN_CLOSE) &&
          ((frm->pse[frm->pse_tos].type == CT_FPAREN_OPEN) ||
           (frm->pse[frm->pse_tos].type == CT_SPAREN_OPEN)))
      {
         pc->type = frm->pse[frm->pse_tos].type + 1;
         if (pc->type == CT_SPAREN_CLOSE)
         {
            frm->sparen_count--;
            pc->flags &= ~PCF_IN_SPAREN;
         }
      }

      /* Make sure the open / close match */
      if (pc->type != (frm->pse[frm->pse_tos].type + 1))
      {
         LOG_FMT(LWARN, "%s:%d Error: Unexpected '%s' for '%s'\n",
                 __func__, pc->orig_line, pc->str,
                 get_token_name(frm->pse[frm->pse_tos].type));
         print_stack(frm, pc);
      }
      else
      {
         /* Copy the parent, update the paren/brace levels */
         pc->parent_type = frm->pse[frm->pse_tos].parent;
         frm->level--;
         if ((pc->type == CT_BRACE_CLOSE) ||
             (pc->type == CT_VBRACE_CLOSE))
         {
            frm->brace_level--;
         }
         pc->level       = frm->level;
         pc->brace_level = frm->brace_level;

         /* Pop the entry */
         frm->pse_tos--;

         /* See if we are in a complex statement */
         if (frm->pse[frm->pse_tos].stage != BS_NONE)
         {
            handle_complex_close(frm, pc);
         }
      }
      return (chunk_get_next(pc));
   }


   /* Get the parent type for brace and paren open */
   parent = CT_NONE;
   if ((pc->type == CT_PAREN_OPEN) ||
       (pc->type == CT_FPAREN_OPEN) ||
       (pc->type == CT_SPAREN_OPEN) ||
       (pc->type == CT_BRACE_OPEN))
   {
      prev = chunk_get_prev_ncnl(pc);
      if (prev != NULL)
      {
         if ((pc->type == CT_PAREN_OPEN) ||
             (pc->type == CT_FPAREN_OPEN) ||
             (pc->type == CT_SPAREN_OPEN))
         {
            /* Set the parent for parens and change paren type */
            if (frm->pse[frm->pse_tos].stage != BS_NONE)
            {
               pc->type = CT_SPAREN_OPEN;
               parent   = frm->pse[frm->pse_tos].type;
               frm->sparen_count++;
            }
            else if (prev->type == CT_FUNCTION)
            {
               pc->type = CT_FPAREN_OPEN;
               parent   = CT_FUNCTION;
            }
            else
            {
               /* no need to set parent */
            }
         }
         else  /* must be CT_BRACE_OPEN */
         {
            /* Set the parent for open braces */
            if (frm->pse[frm->pse_tos].stage != BS_NONE)
            {
               parent = frm->pse[frm->pse_tos].type;
            }
            else if ((prev->type == CT_ASSIGN) && (prev->str[0] == '='))
            {
               parent = CT_ASSIGN;
            }
            else if (prev->type == CT_FPAREN_CLOSE)
            {
               parent = CT_FUNCTION;
            }
            else
            {
               /* no need to set parent */
            }
         }
      }
   }

   /**
    * Adjust the level for opens & create a stack entry
    * Note that CT_VBRACE_OPEN has already been handled.
    */
   if ((pc->type == CT_BRACE_OPEN) ||
       (pc->type == CT_PAREN_OPEN) ||
       (pc->type == CT_FPAREN_OPEN) ||
       (pc->type == CT_SPAREN_OPEN) ||
       (pc->type == CT_SQUARE_OPEN))
   {
      frm->level++;
      if (pc->type == CT_BRACE_OPEN)
      {
         frm->brace_level++;
      }
      frm->pse_tos++;
      frm->pse[frm->pse_tos].type   = pc->type;
      frm->pse[frm->pse_tos].stage  = BS_NONE;
      frm->pse[frm->pse_tos].parent = parent;
      pc->parent_type               = parent;

      print_stack(frm, pc);
   }

   /** Create a stack entry for complex statments IF/DO/FOR/WHILE/SWITCH */
   if ((pc->type == CT_IF) ||
       (pc->type == CT_DO) ||
       (pc->type == CT_FOR) ||
       (pc->type == CT_WHILE) ||
       (pc->type == CT_SWITCH) ||
       (pc->type == CT_BRACED) ||
       (pc->type == CT_VOLATILE))
   {
      frm->pse_tos++;
      frm->pse[frm->pse_tos].type  = pc->type;
      frm->pse[frm->pse_tos].stage = (pc->type == CT_DO) ? BS_BRACE_DO :
                                     ((pc->type == CT_VOLATILE) ||
                                      (pc->type == CT_BRACED)) ? BS_BRACE2 : BS_PAREN1;
      //fprintf(stderr, "opening %s\n", pc->str);

      print_stack(frm, pc);
   }

   /* Mark simple statement/expression starts
    *  - after { or }
    *  - after ';', but not if the paren stack top is a paren
    *  - after '(' that has a parent type of CT_FOR
    */
   if (((pc->type == CT_BRACE_OPEN) && (pc->parent_type != CT_ASSIGN)) ||
       (pc->type == CT_BRACE_CLOSE) ||
       ((pc->type == CT_SPAREN_OPEN) && (pc->parent_type == CT_FOR)) ||
       ((pc->type == CT_SEMICOLON) &&
        (frm->pse[frm->pse_tos].type != CT_PAREN_OPEN) &&
        (frm->pse[frm->pse_tos].type != CT_FPAREN_OPEN) &&
        (frm->pse[frm->pse_tos].type != CT_SPAREN_OPEN)))
   {
      //fprintf(stderr, "%s: %d> reset stmt on %s\n", __func__, pc->orig_line, pc->str);
      frm->stmt_count = 0;
      frm->expr_count = 0;
   }

   /* Mark expression starts */
   if ((pc->type == CT_ARITH) ||
       (pc->type == CT_ASSIGN) ||
       (pc->type == CT_COMPARE) ||
       (pc->type == CT_ANGLE_OPEN) ||
       (pc->type == CT_ANGLE_CLOSE) ||
       (pc->type == CT_RETURN) ||
       (pc->type == CT_GOTO) ||
       (pc->type == CT_CONTINUE) ||
       (pc->type == CT_PAREN_OPEN) ||
       (pc->type == CT_FPAREN_OPEN) ||
       (pc->type == CT_SPAREN_OPEN) ||
       (pc->type == CT_BRACE_OPEN) ||
       (pc->type == CT_SEMICOLON) ||
       (pc->type == CT_COMMA) ||
       (pc->type == CT_COLON) ||
       (pc->type == CT_QUESTION))
   {
      frm->expr_count = 0;
      //fprintf(stderr, "%s: %d> reset expr on %s\n", __func__, pc->orig_line, pc->str);
   }
   return (chunk_get_next(pc));
}


/**
 * Checks the progression of complex statements.
 * - checks for else after if
 * - checks for if after else
 * - checks for while after do
 * - checks for open brace in BRACE2 and BRACE_DO stages, inserts open VBRACE
 * - checks for open paren in PAREN1 and PAREN2 stages, complains
 *
 * @param frm  The parse frame
 * @param pc   The current chunk
 * @return     TRUE - done with this chunk, FALSE - keep processing
 */
static BOOL check_complex_statements(struct parse_frame *frm, chunk_t *pc)
{
   c_token_t  parent;
   chunk_t    *vbrace;

   /* Check for CT_ELSE after CT_IF */
   while (frm->pse[frm->pse_tos].stage == BS_ELSE)
   {
      if (pc->type == CT_ELSE)
      {
         /* Replace CT_IF with CT_ELSE on the stack & we are done */
         frm->pse[frm->pse_tos].type  = CT_ELSE;
         frm->pse[frm->pse_tos].stage = BS_ELSEIF;
         print_stack(frm, pc);
         return(TRUE);
      }

      /* Remove the CT_IF and close the statement */
      frm->pse_tos--;
      close_statement(frm, pc);
   }

   /* Check for CT_IF after CT_ELSE */
   if (frm->pse[frm->pse_tos].stage == BS_ELSEIF)
   {
      if (pc->type == CT_IF)
      {
         /* Replace CT_ELSE with CT_IF */
         frm->pse[frm->pse_tos].type  = CT_IF;
         frm->pse[frm->pse_tos].stage = BS_PAREN1;
         return(TRUE);
      }

      /* Jump to the 'expecting brace' stage */
      frm->pse[frm->pse_tos].stage = BS_BRACE2;
   }

   /* Check for CT_WHILE after the CT_DO */
   if (frm->pse[frm->pse_tos].stage == BS_WHILE)
   {
      if (pc->type == CT_WHILE)
      {
         pc->type                     = CT_WHILE_OF_DO;
         frm->pse[frm->pse_tos].type  = CT_WHILE;
         frm->pse[frm->pse_tos].stage = BS_PAREN2;
         return(TRUE);
      }

      LOG_FMT(LWARN, "%s:%d Error: Expected 'while', got '%s'\n",
              __func__, pc->orig_line, pc->str);
      frm->pse_tos--;
   }

   /* Insert a CT_VBRACE_OPEN, if needed */
   if ((pc->type != CT_BRACE_OPEN) &&
       ((frm->pse[frm->pse_tos].stage == BS_BRACE2) ||
        (frm->pse[frm->pse_tos].stage == BS_BRACE_DO)))
   {
      parent = frm->pse[frm->pse_tos].type;

      vbrace = insert_vbrace_before(pc, frm);
      vbrace->parent_type = parent;

      frm->level++;
      frm->brace_level++;

      frm->pse_tos++;
      frm->pse[frm->pse_tos].type   = CT_VBRACE_OPEN;
      frm->pse[frm->pse_tos].stage  = BS_NONE;
      frm->pse[frm->pse_tos].parent = parent;

      print_stack(frm, pc);

      /* update the level of pc */
      pc->level       = frm->level;
      pc->brace_level = frm->brace_level;

      /* Mark as a start of a statment */
      frm->stmt_count = 0;
      frm->expr_count = 0;
      pc->flags      |= PCF_STMT_START | PCF_EXPR_START;
      frm->stmt_count = 1;
      frm->expr_count = 1;
      //fprintf(stderr, "%d] 2.marked %s as stmt start\n", pc->orig_line, pc->str);
   }

   /* Verify open paren in complex statement */
   if ((pc->type != CT_PAREN_OPEN) &&
       ((frm->pse[frm->pse_tos].stage == BS_PAREN1) ||
        (frm->pse[frm->pse_tos].stage == BS_PAREN2)))
   {
      LOG_FMT(LWARN, "%s:%d Error: Expected '(', got '%s' for '%s'\n",
              __func__, pc->orig_line, pc->str,
              get_token_name(frm->pse[frm->pse_tos].type));

      /* Throw out the complex statement */
      frm->pse_tos--;
   }

   return(FALSE);
}


/**
 * Handles a close paren or brace - just progress the stage, if the end
 * of the statement is hit, call close_statement()
 *
 * @param frm  The parse frame
 * @param pc   The current chunk
 */
static void handle_complex_close(struct parse_frame *frm, chunk_t *pc)
{
   if (frm->pse[frm->pse_tos].stage == BS_PAREN1)
   {
      /* PAREN1 always => BRACE2 */
      frm->pse[frm->pse_tos].stage = BS_BRACE2;
   }
   else if (frm->pse[frm->pse_tos].stage == BS_BRACE2)
   {
      /* BRACE2: IF => ELSE, anyting else => close */
      if (frm->pse[frm->pse_tos].type == CT_IF)
      {
         frm->pse[frm->pse_tos].stage = BS_ELSE;
      }
      else
      {
         frm->pse_tos--;
         close_statement(frm, pc);
      }
   }
   else if (frm->pse[frm->pse_tos].stage == BS_BRACE_DO)
   {
      frm->pse[frm->pse_tos].stage = BS_WHILE;
   }
   else if (frm->pse[frm->pse_tos].stage == BS_PAREN2)
   {
      frm->pse_tos--;
      close_statement(frm, pc);
   }
   else
   {
      /* PROBLEM */
      LOG_FMT(LWARN, "%s:%d Error: TOS.type='%s' TOS.stage=%d\n",
              __func__, pc->orig_line,
              get_token_name(frm->pse[frm->pse_tos].type),
              frm->pse[frm->pse_tos].stage);
   }
   print_stack(frm, pc);
}


static chunk_t *insert_vbrace(chunk_t *pc, BOOL after,
                              struct parse_frame *frm)
{
   chunk_t chunk;
   chunk_t *rv;
   chunk_t *ref;

   memset(&chunk, 0, sizeof(chunk));

   //   fprintf(stderr, "%s_%s: %s on line %d\n",
   //           __func__, after ? "after" : "before",
   //           get_token_name(pc->type), pc->orig_line);

   chunk.len         = 0;
   chunk.orig_line   = pc->orig_line;
   chunk.parent_type = frm->pse[frm->pse_tos].type;
   chunk.level       = frm->level;
   chunk.brace_level = frm->brace_level;
   chunk.flags       = pc->flags & PCF_COPY_FLAGS;
   if (after)
   {
      chunk.type = CT_VBRACE_CLOSE;
      rv         = chunk_add_after(&chunk, pc);
   }
   else
   {
      ref = chunk_get_prev(pc);
      while (chunk_is_newline(ref) || chunk_is_comment(ref))
      {
         ref = chunk_get_prev(ref);
      }
      chunk.orig_line = ref->orig_line;
      chunk.column    = ref->column + ref->len + 1;
      chunk.type      = CT_VBRACE_OPEN;
      rv              = chunk_add_after(&chunk, ref);
   }
   return(rv);
}


/**
 * Called when a statement was just closed and the pse_tos was just
 * decremented.
 *
 * - if the TOS is now VBRACE, insert a CT_VBRACE_CLOSE and recurse.
 * - if the TOS is a complex statement, call handle_complex_close()
 */
void close_statement(struct parse_frame *frm, chunk_t *pc)
{
   chunk_t *vbc = pc;

   LOG_FMT(LTOK, "%s:%d] %s'%s' type %s stage %d\n", __func__,
           pc->orig_line,
           get_token_name(pc->type), pc->str,
           get_token_name(frm->pse[frm->pse_tos].type),
           frm->pse[frm->pse_tos].stage);

   /*TODO: not sure I follow this */
//   if (pc->type != CT_VBRACE_CLOSE)
//   {
//      frm->expr_count = 1;
//      if (frm->pse[frm->pse_tos].type != CT_SPAREN_OPEN)
//      {
//         frm->stmt_count = 1;
//      }
//   }

   /* If we are in a virtual brace -- close it */
   if (frm->pse[frm->pse_tos].type == CT_VBRACE_OPEN)
   {
      frm->level--;
      frm->brace_level--;
      frm->pse_tos--;

      //fprintf(stderr, "%2d> CS1: closed on %s\n", pc->orig_line, get_token_name(pc->type));

      print_stack(frm, pc);

      vbc = insert_vbrace_after(pc, frm);

      frm->stmt_count = 1;
      frm->expr_count = 1;
      handle_complex_close(frm, vbc);
      return;
   }

   /* See if we are done with a complex statement */
   if (frm->pse[frm->pse_tos].stage != BS_NONE)
   {
      handle_complex_close(frm, vbc);
      print_stack(frm, pc);
      //fprintf(stderr, "%2d> CS1: closed on %s\n", pc->orig_line, get_token_name(pc->type));
   }
}

