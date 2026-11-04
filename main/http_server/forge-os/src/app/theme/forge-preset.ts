import Aura from '@primeng/themes/aura';

// Same deep merge PrimeNG's definePreset does. Imported locally because the
// `@primeng/themes` root typings need `moduleResolution: bundler`.
type Plain = Record<string, any>;
const isPlain = (v: unknown): v is Plain => !!v && typeof v === 'object' && !Array.isArray(v);
function mergePreset(base: Plain, override: Plain): Plain {
  const out: Plain = { ...base };
  for (const [key, value] of Object.entries(override)) {
    out[key] = isPlain(value) && isPlain(base[key]) ? mergePreset(base[key], value) : value;
  }
  return out;
}

// PrimeNG tokens for the forge look: steel for anything interactive,
// navy surfaces matching the app shell. Heat colours are reserved for
// temperature data and never used for controls (see utils/heat.ts).
export const ForgePreset = mergePreset(Aura as Plain, {
  semantic: {
    primary: {
      50: '#f3f6f9',
      100: '#e3e9f0',
      200: '#c9d4e0',
      300: '#adbfd0',
      400: '#8fa6bf',
      500: '#7690ad',
      600: '#5f7896',
      700: '#4c607a',
      800: '#3a4a5e',
      900: '#2a3645',
      950: '#18202a',
    },
    borderRadius: {
      none: '0',
      xs: '2px',
      sm: '3px',
      md: '4px',
      lg: '6px',
      xl: '8px',
    },
    colorScheme: {
      dark: {
        surface: {
          0: '#ffffff',
          50: '#e6edf5',
          100: '#c8d2de',
          200: '#a7b4c4',
          300: '#8a9bb0',
          400: '#6f8198',
          500: '#56677d',
          600: '#3f4e61',
          700: '#2a3a4c',
          800: '#17222f',
          900: '#0b1219',
          950: '#070d17',
        },
        formField: {
          background: 'transparent',
          borderColor: '{surface.700}',
          hoverBorderColor: '{surface.500}',
          color: '{surface.50}',
          placeholderColor: '{surface.400}',
          shadow: 'none',
        },
        text: {
          color: '{surface.50}',
          hoverColor: '{surface.0}',
          mutedColor: '{surface.300}',
          hoverMutedColor: '{surface.200}',
        },
      },
    },
  },
});
