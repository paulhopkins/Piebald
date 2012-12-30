#include <stdio.h>
#include <string.h>

#define MAX_CELLS 32000
#define MAX_LOC   10000

typedef unsigned char cell_t;

struct loop {
  unsigned int start_row;
  unsigned int cell;
  unsigned int end_row;
};

int read_code(char *filename, char *code[], int* locp);
int run(char *code[], int loc);

int main(int argc, char *argv[])
{
  int		sts = 0;
  char		*code[MAX_LOC];
  int           loc;

  if (argc != 2) {
    printf("Usage: %s filename\n", argv[0]);
    return 1;
  }
  
  sts = read_code(argv[1], code, &loc);
  
  if (!sts)
    {
      sts = run(code, loc);
    }

  return sts;
}

int read_code(char *filename, char *code[], int *locp)
{
  int sts = 0;
  FILE *file = fopen(filename, "r");
  char line[MAX_CELLS];
  int row = 0;
  int syntax_error = 0;
  char *p;

  if (file == 0) {
    printf("Could not open file\n");
    sts = 1;
  } else {
    while ( fgets(line, MAX_CELLS, file) ) {
      p = strchr(line, '\n');

      if (p && *p) {
	*p = '\0';
      }

      p = strchr(line, '#');
      if (p) {
	*p = '\0';
      }

      p = strchr(line, '?');
      if (p) {
	if (strchr(p+1, '?')) syntax_error = 1;
	if (strchr(line, ':')) syntax_error = 1;
      }

      p = strchr(line, ':');
      if (p) {
	if (strchr(p+1, ':')) syntax_error = 1;
	if (strchr(line, '?')) syntax_error = 1;
      }

      if (syntax_error)
	{
	  printf("Multiple branches cannot start or end on the same line\n");
	  sts = 1;
	  break;
	}
	    

      if (line && *line)
	{
	  code[row++] = strdup(line);
	}
    }

    *locp = row;

  }

  return sts;
}

int run(char *code[], int loc)
{
  int row = 0;
  cell_t	cells[MAX_CELLS][2];
  int cell = 0;
  char c;
  int src = 0, dst = 1;
  int new_row = 0;

  struct loop loops[100];
  int nloop = 0, loop;

  while (row < loc)
    {
      cell = 0;
            while((c = code[row][cell]) != '\0')
	      {
		if (c == '?')
		  {
		    loops[nloop].start_row = row;
		    loops[nloop].cell = cell;
		    loops[nloop].end_row = row;
		    nloop++;
		  }
		else if (c == ':')
		  {
		    loop = nloop;
		    while(loop--)
		      {
			if(loops[loop].cell == cell)
			  {
			    loops[loop].end_row = row;
			    break;
			  }
		      }
		    if (loop == -1) {
		      printf("Unmatched ':'\n");
		      return 1;
		    }
		  }
		cell++;
	      }
	    row++;
    }
  
  for (loop = 0; loop < nloop; loop++)
    {
      if (loops[loop].start_row == loops[loop].end_row)
	{
	  printf("Syntax Error: Unmatched '?'\n");
	  return 1;
	}
    }

  row = 0;
  while(row < loc)
    {
      cell = 0;
      new_row = row + 1;
      while((c = code[row][cell]) != '\0')
	{
	  cells[cell][dst] = cells[cell][src];

	  if (c >= '0' && c <= '7')
	    {
	      cells[cell][dst] = cells[cell][src] | (1 << (c - '0'));
	    }

	  else if (c == '!')
	    {
	      cells[cell][dst] = 0;
	    }

	  else if (c == '.')
	    {
	      putchar(cells[cell][dst]);
	    }

	  else if (c == '@')
	    {
	      printf("%d", cells[cell][dst]);
	    }

	  else if (c == ',')
	    {
	      cells[cell][dst] = getchar();
	    }

	  else if (c == '^')
	    {
	      cells[cell][dst] = cells[cell][src] ^
		cells[(cell+1) % MAX_CELLS][src];
	    }

	  else if (c == '|')
	    {
	      cells[cell][dst] = cells[cell][src] |
		cells[(cell+1) % MAX_CELLS][src];
	    }

	  else if (c == '&')
	    {
	      cells[cell][dst] = cells[cell][src] &
	      cells[(cell + 1) % MAX_CELLS][src];
	    }

	  else if (c == '~')
	    {
	      cells[cell][dst] = ~cells[cell][src];
	    }

	  else if (c == '<')
	    {
	      cells[cell][dst] = cells[cell][src] << 1;
	    }

	  else if (c == '>')
	    {
	      cells[cell][dst] = cells[cell][src] >> 1;
	    }

	  else if (c == '}')
	    {
	      cells[cell][dst] = cells[(cell - 1) % MAX_CELLS][src];
	    }

	  else if (c == '{')
	    {
	      cells[cell][dst] = cells[(cell + 1) % MAX_CELLS][src];
	    }

	  else if (c == '?' && !cells[cell][src])
	    {
	    for (loop = 0; loop < nloop; loop++)
	      {
		if (loops[loop].cell == cell && loops[loop].start_row == row)
		  {
		    new_row = loops[loop].end_row + 1;
		    break;
		  }
	      }
	    }

	  else if (c == ':' && cells[cell][src])
	    {
	    for (loop = 0; loop < nloop; loop++)
	      {
		if (loops[loop].cell == cell && loops[loop].end_row == row)
		  {
		    new_row = loops[loop].start_row;
		    break;
		  }
	      }
	    }
		

	  cell++;
	}

      cell = 0;
      while((c = code[row][cell]) != '\0')
	{
	  cells[cell][src] = cells[cell][dst];
	  cell++;
	}
      
      row = new_row;

      dst = src;
      src = (src + 1) % 2;
    }

  return 0;
}
