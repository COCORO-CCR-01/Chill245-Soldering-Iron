#include "fmt.h"
#include <stdarg.h>
#include <stddef.h>

static uint8_t fmt_utoa(unsigned long value, char *text)
{
  char reverse[16];
  uint8_t count = 0U;
  uint8_t i;

  if(value == 0UL)
  {
    text[0] = '0';
    text[1] = '\0';
    return 1U;
  }

  while((value != 0UL) && (count < sizeof(reverse)))
  {
    reverse[count] = (char)('0' + (value % 10UL));
    value /= 10UL;
    count++;
  }

  for(i = 0U; i < count; i++)
  {
    text[i] = reverse[count - 1U - i];
  }
  text[count] = '\0';

  return count;
}

static void fmt_put(char *buffer, uint32_t size, uint32_t *pos, char ch)
{
  if((*pos + 1U) < size)
  {
    buffer[*pos] = ch;
  }
  (*pos)++;
}

static void fmt_put_padded(char *buffer, uint32_t size, uint32_t *pos,
                           const char *text, uint32_t width, char pad)
{
  uint32_t len = 0U;
  uint32_t i;

  while(text[len] != '\0')
  {
    len++;
  }

  while(width > len)
  {
    fmt_put(buffer, size, pos, pad);
    width--;
  }

  for(i = 0U; i < len; i++)
  {
    fmt_put(buffer, size, pos, text[i]);
  }
}

int fmt_snprintf(char *buffer, uint32_t size, const char *format, ...)
{
  va_list args;
  uint32_t pos = 0U;
  char number[24];
  char one[2];
  const char *text;
  uint32_t width;
  char pad;
  uint8_t is_long;
  char spec;
  long signed_value;
  unsigned long unsigned_value;

  if((buffer == NULL) || (size == 0U) || (format == NULL))
  {
    return 0;
  }

  va_start(args, format);

  while(*format != '\0')
  {
    if(*format != '%')
    {
      fmt_put(buffer, size, &pos, *format++);
      continue;
    }

    format++;
    pad = ' ';
    if(*format == '0')
    {
      pad = '0';
      format++;
    }

    width = 0U;
    while((*format >= '0') && (*format <= '9'))
    {
      width = (width * 10U) + (uint32_t)(*format - '0');
      format++;
    }

    is_long = 0U;
    if(*format == 'l')
    {
      is_long = 1U;
      format++;
    }

    spec = *format++;
    one[1] = '\0';
    text = one;

    switch(spec)
    {
      case 's':
        text = va_arg(args, const char *);
        if(text == NULL)
        {
          text = "";
        }
        fmt_put_padded(buffer, size, &pos, text, width, pad);
        break;

      case 'c':
        one[0] = (char)va_arg(args, int);
        fmt_put_padded(buffer, size, &pos, one, width, pad);
        break;

      case 'd':
        signed_value = is_long ? va_arg(args, long) : (long)va_arg(args, int);
        if(signed_value < 0L)
        {
          number[0] = '-';
          fmt_utoa((unsigned long)(-signed_value), &number[1]);
        }
        else
        {
          fmt_utoa((unsigned long)signed_value, number);
        }
        fmt_put_padded(buffer, size, &pos, number, width, pad);
        break;

      case 'u':
        unsigned_value = is_long ?
                         va_arg(args, unsigned long) :
                         (unsigned long)va_arg(args, unsigned int);
        fmt_utoa(unsigned_value, number);
        fmt_put_padded(buffer, size, &pos, number, width, pad);
        break;

      case '%':
        one[0] = '%';
        fmt_put_padded(buffer, size, &pos, one, width, pad);
        break;

      default:
        one[0] = spec;
        fmt_put_padded(buffer, size, &pos, one, width, pad);
        break;
    }
  }

  va_end(args);

  if(pos >= size)
  {
    buffer[size - 1U] = '\0';
  }
  else
  {
    buffer[pos] = '\0';
  }

  return (int)pos;
}
