class Lci < Formula
  desc "Lightning Code Index - fast semantic code search for AI assistants"
  homepage "https://github.com/standardbeagle/lci"
  url "https://github.com/standardbeagle/lci/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "PLACEHOLDER_SHA256"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "ninja" => :build

  def install
    system "cmake", "-S", ".", "-B", "build",
           "-G", "Ninja",
           "-DCMAKE_BUILD_TYPE=Release",
           *std_cmake_args
    system "cmake", "--build", "build", "--parallel"
    bin.install "build/src/lci"
  end

  test do
    assert_match "lci version", shell_output("#{bin}/lci --version")
  end
end
