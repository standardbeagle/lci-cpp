package receivers

type Mux struct {
	routes map[string]string
}

func (m *Mux) ServeHTTP(path string) string {
	return m.routes[path]
}

func (m *Mux) Register(path, handler string) {
	m.routes[path] = handler
}

func NewMux() *Mux {
	return &Mux{routes: map[string]string{}}
}
