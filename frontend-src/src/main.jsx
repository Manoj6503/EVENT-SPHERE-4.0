import {createRoot} from 'react-dom/client';import App from './App.jsx';import './styles.css';
createRoot(document.getElementById('root')).render(<App/>);
// installable + offline app shell (HTTPS or localhost only; the browser enforces this)
if(import.meta.env.PROD&&'serviceWorker' in navigator)addEventListener('load',()=>navigator.serviceWorker.register('/sw.js').catch(()=>{}));
