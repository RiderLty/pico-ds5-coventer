import React, { useEffect, useMemo } from 'react';
import { ThemeProvider, createTheme } from '@mui/material/styles';
import LuaWorkspace from './components/LuaWorkspace';
import { useThemeMode, watchSystemTheme } from './utils/themeStore';
import { deviceSession } from './utils/deviceSession';

export default function App() {
    const { resolved } = useThemeMode();
    useEffect(watchSystemTheme, []);
    useEffect(() => deviceSession.watchUsb(), []);
    const theme = useMemo(() => createTheme({
        palette: { mode: resolved, primary: { main: resolved === 'dark' ? '#86b7ff' : '#0b4fa8' } },
        typography: { fontFamily: 'inherit', button: { textTransform: 'none', fontSize: 12 } },
        components: {
            MuiButton: { defaultProps: { disableElevation: true, size: 'small' } },
            MuiDialog: { styleOverrides: { paper: { backgroundImage: 'none' } } },
        },
    }), [resolved]);
    return <ThemeProvider theme={theme}><LuaWorkspace /></ThemeProvider>;
}
