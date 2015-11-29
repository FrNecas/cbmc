#include <assert.h>

struct str
{
  int x;
  int y;
  int z;
};

void pass_through_struct (struct str *s, int q)
{
  s->x = q;
  s->y = s->x;
  s->z = s->y;

  return s;
}

int main (void)
{
  int q;

  struct str s;

  pass_through_struct(&s,q);

  assert(q == s.z);

  return 1;
}
