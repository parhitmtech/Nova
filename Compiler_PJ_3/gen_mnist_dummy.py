# save as gen_mnist_dummy.py
# run: python gen_mnist_dummy.py
import numpy as np

np.random.seed(42)
N = 200        # samples
FEATURES = 784 # same as MNIST (28x28)
CLASSES = 10   # same as MNIST (digits 0-9)

# generate 200 samples — each class gets 20 samples
# each class has a distinct "pattern" so the network can learn
X_rows = []
Y_rows = []

for cls in range(CLASSES):
    for _ in range(N // CLASSES):
        # base pattern: class i has high values in feature region i
        x = np.random.rand(FEATURES) * 0.3        # low noise background
        start = cls * (FEATURES // CLASSES)
        end   = start + (FEATURES // CLASSES)
        x[start:end] += 0.7                        # strong signal in class region
        x = np.clip(x, 0.0, 1.0)                  # keep in [0, 1]
        X_rows.append(x)

        # one-hot label
        y = np.zeros(CLASSES)
        y[cls] = 1.0
        Y_rows.append(y)

X = np.array(X_rows)
Y = np.array(Y_rows)

# shuffle
idx = np.random.permutation(N)
X, Y = X[idx], Y[idx]

np.savetxt('mnist_X.csv', X, delimiter=',', fmt='%.6f')
np.savetxt('mnist_Y.csv', Y, delimiter=',', fmt='%d')
print(f"Done — mnist_X.csv ({N}x{FEATURES}), mnist_Y.csv ({N}x{CLASSES})")