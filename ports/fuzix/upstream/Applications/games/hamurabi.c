/*
  The MIT License

  Copyright (c) 2010 Brian L. Troutwine
  
  Marginally rearranged for Fuzix by Alan Cox 2018

  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated
  documentation files (the "Software"), to deal in the
  Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software,
  and to permit persons to whom the Software is furnished to
  do so, subject to the following conditions:

  The above copyright notice and this permission notice shall
  be included in all copies or substantial portions of the
  Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
  KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
  WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
  PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
  OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
  OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
  OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <ctype.h>
#include <string.h>

/* Ripple: wide resource accounting, checked input/EOF/q, and corrected
 * starvation, harvest losses and yearly allocation bookkeeping. */

#ifdef USE_FLOAT
#define RAND(N) (uint32_t)((double)rand() / ((double)RAND_MAX + 1) * N)
#else
#define RAND(N) int_random(N)
#endif

typedef struct _st city_st;
typedef enum _resp {
  OKAY,     /* Is there a problem here? There's no problem here. */
  EACREAGE, /* Insufficient acres in city. */
  EPOP,     /* Insufficient population in city. */
  EBUSHELS, /* Insufficient bushels in city storage. */
  ESTARVE   /* More than 45% of people starved in a year. */
} resp;

city_st *new_city_st(void);
void free_city_st(city_st *cty);
void add_migrants(city_st *cty);
void rats(city_st *cty);
uint32_t plague(city_st *cty);
uint32_t starvation(city_st *cty);
uint32_t births(city_st *cty);
resp step(city_st *cty);
resp buy_acres(city_st *cty, const uint32_t amnt);
resp sell_acres(city_st *cty, const uint32_t amnt);
resp feed_populace(city_st *cty, const uint32_t amnt);
resp plant_seed(city_st *cty, const uint32_t amnt);

uint32_t population(const city_st *cty);
uint32_t acres(const city_st *cty);
uint32_t yield(const city_st *cty);
uint32_t pests(const city_st *cty);
uint32_t bushels(const city_st *cty);
uint32_t trade_val(const city_st *cty);
uint32_t starved(const city_st *cty);
uint32_t migrated(const city_st *cty);
uint32_t tot_died(const city_st *cty);
uint32_t avg_starved(const city_st *cty);

struct _st {
  uint32_t population;
  uint32_t starved;
  uint32_t migrated;
  uint32_t bushels;
  uint32_t acres;
  uint32_t yield;
  uint32_t pests;
  uint32_t trade_val;
  uint32_t avg_starved;
  uint32_t tot_died;
  uint32_t fed;
  uint32_t planted;
};

uint32_t int_random(uint32_t n)
{
   return ((unsigned long)rand() >> 4) % n;
}

city_st *new_city_st(void) {
  city_st *cty = (city_st *) malloc(sizeof(city_st));
  if (cty == NULL) {
    fprintf(stderr, "Out of memory.\n");
    exit(EXIT_FAILURE);
  }

  cty->population  = 95;
  cty->migrated    = 5;
  cty->starved     = 0;
  cty->bushels     = 2800;
  cty->acres       = 1000;
  cty->yield       = 3;
  cty->pests       = 200;
  cty->trade_val   = 17+RAND(10);
  cty->avg_starved = 0;
  cty->tot_died    = 0;
  cty->fed         = 0;
  cty->planted     = 0;

  return cty;
}

void free_city_st(city_st *cty) {
  free(cty);
}

void add_migrants(city_st *cty) {
  if (cty->migrated > 6000 - cty->population)
    cty->migrated = 6000 - cty->population;
  cty->population += cty->migrated;
  cty->migrated = 0;
}

void rats(city_st *cty) {
  cty->pests = cty->bushels / (RAND(5)+2);
  cty->bushels -= cty->pests;
}

uint32_t plague(city_st *cty) {
  uint32_t died = cty->population / (RAND(4)+2);
  cty->population -= died;
  return died;
}

uint32_t starvation(city_st *cty) {
  /* 20 bushels of grain are needed for each citizen to
     live. */
  cty->starved = cty->fed / 20 >= cty->population
      ? 0 : cty->population - cty->fed / 20;
  cty->population -= cty->starved;
  return cty->starved;
}

uint32_t births(city_st *cty) {
  /* For each two people living at the end of the year, one
     child is born. Carrying capacity of city is 6000. */
  uint32_t pop = cty->population;
  uint32_t born = pop / 2;
  return born > 6000 - pop ? 6000 - pop : born;
}

resp step(city_st *cty) {
  uint32_t died = 0, before = cty->population;
  died += starvation(cty);
  if (died > ((before * 9) / 20))
    return ESTARVE;
  cty->avg_starved = (died + 9*cty->avg_starved)/10; // EMA

  if (RAND(15) == 0)
    died += plague(cty);
  cty->tot_died += died;

  cty->population += births(cty);

  cty->migrated  = RAND(cty->population)/10;
  cty->trade_val = 17+RAND(10);
  cty->yield = RAND(10)+1;

  cty->bushels += cty->yield*cty->planted;
  rats(cty);
  return OKAY;
}

resp buy_acres(city_st *cty, const uint32_t amnt) {
  if (cty->trade_val*amnt > cty->bushels)
    return EBUSHELS;
  cty->bushels -= (cty->trade_val)*amnt;
  cty->acres   += amnt;
  return OKAY;
}

resp sell_acres(city_st *cty, const uint32_t amnt) {
  if (amnt > cty->acres)
    return EACREAGE;
  cty->bushels += (cty->trade_val)*amnt;
  cty->acres   -= amnt;
  return OKAY;
}

resp feed_populace(city_st *cty, const uint32_t amnt) {
  if (amnt > cty->bushels)
    return EBUSHELS;
  cty->bushels -= amnt;
  cty->fed = amnt;
  return OKAY;
}

resp plant_seed(city_st *cty, const uint32_t amnt) {
  if (amnt > cty->acres)
    return EACREAGE;
  else if (amnt > cty->bushels)
    return EBUSHELS;
  else if (amnt > 10*cty->population)
    return EPOP;

  cty->bushels -= amnt;
  cty->planted = amnt;
  return OKAY;
}

uint32_t population(const city_st *cty) {
  return cty->population;
}

uint32_t acres(const city_st *cty) {
  return cty->acres;
}

uint32_t yield(const city_st *cty) {
  return cty->yield;
}

uint32_t pests(const city_st *cty) {
  return cty->pests;
}

uint32_t bushels(const city_st *cty) {
  return cty->bushels;
}

uint32_t trade_val(const city_st *cty) {
  return cty->trade_val;
}

uint32_t starved(const city_st *cty) {
  return cty->starved;
}

uint32_t migrated(const city_st *cty) {
  return cty->migrated;
}

uint32_t tot_died(const city_st *cty) {
  return cty->tot_died;
}

uint32_t avg_starved(const city_st *cty) {
  return cty->avg_starved;
}

long input(void);

void retire(void);
void storm_out(void);
void ejected(void);
void game_end(const city_st *cty);
void nero_end(void);
void not_so_bad_end(const city_st *cty);

long input(void) {
  char text[40], *end;
  long value;
  int ch;
  for (;;) {
    fflush(stdout);
    if (!fgets(text, sizeof(text), stdin)) retire();
    if (!strchr(text, '\n') && !feof(stdin)) {
      while ((ch = getchar()) != '\n' && ch != EOF) {}
      puts("Please enter a whole number, or q to quit.");
      continue;
    }
    end = text;
    while (isspace(*end)) end++;
    if (*end == 'q' || *end == 'Q') retire();
    errno = 0;
    value = strtol(text, &end, 10);
    if (end == text) { puts("Please enter a whole number, or q to quit."); continue; }
    while (isspace(*end)) end++;
    if (errno || *end || value > 1000000) {
      puts("Please enter a whole number up to 1000000, or q to quit.");
      continue;
    }
    if (value < 0) storm_out();
    return value;
  }
}

/*
   Client functions

   Someday I dream of being in curses.
 */
void retire(void) {
  printf("So long for now.\n");
  exit(EXIT_SUCCESS);
}

void storm_out(void) {
  printf("Hamurabi: I cannot do what you wish.\n");
  printf("Get yourself another steward!!!!!\n");
  retire();
}

void ejected(void) {
  printf("Due to this extreme mismanagement you have not only\n");
  printf("been impeached and thrown out of office but you have\n");
  printf("also been declared 'National Fink' !!\n");
  retire();
}

void game_end(const city_st *cty) {
  uint32_t l = (uint32_t) (acres(cty)/(population(cty)));
  printf("In your 10-year term of office %lu percent of the\n",
         avg_starved(cty));
  printf("population starved per year on average, i.e., a total of\n");
  printf("%lu people died!!\n\n", tot_died(cty));
  printf("You started with 10 acres per person and ended with\n");
  printf("%lu acres per person.\n\n", l);

  if ((avg_starved(cty)>33) || (l<7)) ejected();
  if ((avg_starved(cty)>10) || (l<9)) nero_end();
  if ((avg_starved(cty)>3)  || (l<10)) not_so_bad_end(cty);
}

void nero_end(void) {
  printf("Your heavy handed performance smacks of Nero and Ivan IV.\n");
  printf("The people (remaining) find you an unpleasant ruler, and,\n");
  printf("frankly, hate your guts!\n");
  retire();
}

void not_so_bad_end(const city_st *cty) {
  printf("Your performance could have been somewhat better, but\n");
  printf("really wasn't too bad at all. ");
  printf("%lu people would ", RAND(population(cty)));
  printf("dearly like to see you assassinated but we all have our\n");
  printf("trivial problems.\n");
  retire();
}

int main(int argc, char **argv) {

  city_st *cty;
  long inp;
  uint32_t year;
  if (argc > 1) {
    puts("Usage: hamurabi\nGovern Sumeria for ten years; q or Ctrl-C quits.");
    return strcmp(argv[1], "--help") ? 1 : 0;
  }
  srand((unsigned)time(NULL));
  cty = new_city_st();

  puts("Try your hand at governing Ancient Sumeria\n"
       "successfully for a 10 year term of office.\nEnter q to quit.\n");
  for (year = 1; year <= 10; year++) {
    cty->fed = cty->planted = 0;
    printf("Hamurabi: I beg to report to you,\nin year %lu, ", year);
    printf("%lu people starved %lu came to the city.\n",
           starved(cty), migrated(cty));

    add_migrants(cty);

    if (RAND(2) == 1) {
      printf("A horrible plague struck! Many people have died!\n");
      plague(cty);
    }

    printf("\nPopulation is now %lu\n", population(cty));
    printf("The city now owns %lu acres\n", acres(cty));
    printf("You have harvested %lu bushels per acre.\n",
           yield(cty));
    printf("Rats ate %lu bushels.\n\n",
           pests(cty));
    printf("You now have %lu bushels in store.\n\n",
           bushels(cty));
    printf("Land is trading at %lu bushels per acre.\n",
           trade_val(cty));
  buy_acres:
    printf("How many acres do you wish to buy?\n");
    inp = input();
    if (inp < 0) storm_out();
    else if (inp == 0) goto sell_acres;
    if (buy_acres(cty, inp) != 0) {
      printf("Hamurabi: Think again. You have only %lu",
             bushels(cty));
      printf(" bushels of grain. Now then,\n");
      goto buy_acres;
    }
    goto feed_people;

  sell_acres:
    printf("How many acres do you wish to sell?\n");
    inp = input();
    if (inp < 0) storm_out();
    else if (inp == 0) goto feed_people;
    if (sell_acres(cty, inp) != 0) {
      printf("Hamurabi: Think again. You have only %lu acres.",
             acres(cty));
      printf(" Now then,\n");
      goto sell_acres;
    }

  feed_people:
    printf("How many bushels do you wish to feed your people?\n");
    printf("(No starvation: %lu bushels.)\n",
           population(cty)*20);
    inp = input();
    if (inp < 0) storm_out();
    else if (inp == 0) goto plant_seed;
    if (feed_populace(cty, inp) != 0) {
      printf("Hamurabi: Think again. We only have %lu bushels.",
             bushels(cty));
      printf(" Now then,\n");
      goto feed_people;
    }

  plant_seed:
    printf("How many acres do you wish to plant with seed?\n");
    printf("(MAX: %lu acres)\n", 10*population(cty));
    inp = input();
    if (inp < 0) storm_out();
    else if (inp == 0) goto progress;
    switch(plant_seed(cty, inp)) {
    case EACREAGE: {
      printf("Hamurabi: Think again. You have only %lu acres.",
             acres(cty));
      printf(" Now then,\n");
      goto plant_seed;
    } break;
    case EPOP: {
      printf("But you only have %lu people to tend the fields.",
             population(cty));
      printf(" Now then,\n");
      goto plant_seed;
    } break;
    case EBUSHELS: {
      printf("Hamurabi: Think again. We only have %lu bushels.",
             bushels(cty));
      printf(" Now then,\n");
      goto plant_seed;
    } break;
    case ESTARVE:
    case OKAY: break;
    }

  progress:
    if (step(cty) == ESTARVE) {
      printf("You starved %lu people in one year!!!\n",
             starved(cty));
      ejected();
    }

  }

  game_end(cty);
  free_city_st(cty);
  return 0;
}
